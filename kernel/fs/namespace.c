// namespace.c - Per-Process Namespace (Plan 9 bind semantics)
 
#include "namespace.h"
#include "vfs.h"
#include "kheap.h"
#include "string.h"
#include "kprintf.h"

// count # binds at path
static uint32_t count_at(const ns_t *ns, const char *new_path) {

    uint32_t n = 0;

    for (uint32_t i = 0; i < NS_BINDS_MAX; i++) {
        if (ns->binds[i].active && strcmp(ns->binds[i].new_path, new_path) == 0)
        n++;
    }
    return n;
}

// allocate ns object
ns_t *ns_create(void) {
 
    ns_t *ns = kmalloc(sizeof(ns_t));                               // alloc mem
    if (!ns) {
        kprintf("NS: ns_create - OOM\n");                           // OOM
        return 0;
    }
 
    // initialise fields
    ns->refcount = 1;
    ns->nbinds   = 0;
 
    for (uint32_t i = 0; i < NS_BINDS_MAX; i++) {                   // clear bind table
        ns->binds[i].active = 0;
        ns->binds[i].vnode  = 0;
        ns->binds[i].srv_fd = -1;
    }
    kprintf("NS: created namespace @ 0x%p\n", (uint32_t)ns);
    return ns;
}

// deep copy ns
ns_t *ns_clone(const ns_t *src) {
 
    ns_t *dst = ns_create();                                        // alloc ns
    if (!dst) return 0;
 
    if (!src) return dst;                                           // cloning null = fresh ns
 
    dst->nbinds = src->nbinds;                                      // copy bind count
 
    for (uint32_t i = 0; i < NS_BINDS_MAX; i++) {                   // loop bind table
 
        const ns_bind_entry_t *se = &src->binds[i];
        ns_bind_entry_t       *de = &dst->binds[i];
 
        if (!se->active) {                                          // case 1: entry inactive
            de->active = 0;
            de->vnode  = 0;
            de->srv_fd = -1;
            continue;
        }
 
        strncpy(de->new_path, se->new_path, VFS_PATH_MAX - 1);      // case 2: entry active
        de->new_path[VFS_PATH_MAX - 1] = '\0';
 
        de->vnode  = se->vnode;                                     // copy -> path
        de->flags  = se->flags;                                     // copy -> flags
        de->active = 1;                                             // copy -> vnode-ptr
        de->srv_fd = se->srv_fd;                                    // copy -> srv_fd
 
        if (de->vnode) vnode_ref(de->vnode);                        // vnode_ref
    }
    kprintf("NS: cloned namespace 0x%p -> 0x%p (%u binds)\n", (uint32_t)src, (uint32_t)dst, dst->nbinds);
    return dst;
}

// increment refcount
void  ns_ref  (ns_t *ns) {
    if (ns) ns->refcount++;
}

// decrement refcount (refcount = 0 = free ns)
void  ns_unref(ns_t *ns) {

    if (!ns) return;
    if (ns->refcount == 0) {                                                // prevent double free
        kprintf("NS: ns_unref - WARNING: refcount already zero\n");
        return;
    }
 
    ns->refcount--;                                                         // decrement
    if (ns->refcount > 0) return;                                           // if still shared, do nothing
 
    for (uint32_t i = 0; i < NS_BINDS_MAX; i++) {                           // release all vnode refs
        if (ns->binds[i].active && ns->binds[i].vnode) {
            vnode_unref(ns->binds[i].vnode);
            ns->binds[i].vnode = 0;
        }
    }
    kprintf("NS: freed namespace @ 0x%p\n", (uint32_t)ns);
    kfree(ns);                                                              // free
}

// copy on bind
static int ns_cow(ns_t **ns) {

    if (!ns || !*ns) return -1;                     // validation
    if ((*ns)->refcount <= 1) return 0;             // shared? check
 
    ns_t *fresh = ns_clone(*ns);                    // clone ns
    if (!fresh) return -1;
 
    ns_unref(*ns);                                  // drop old reference
    *ns = fresh;                                    // replace pointer
    return 0;

}

// bind operation
int ns_bind(ns_t **nsp, vnode_t *vnode, const char *new_path, uint8_t flags) {
 
    if (!nsp || !*nsp || !vnode || !new_path || new_path[0] != '/') return -1;          // validate inputs
 
    if (ns_cow(nsp) < 0) return -1;                                                     // copy on write (save mutation)
    ns_t *ns = *nsp;
 
    if (flags == NS_BIND_REPLACE) {                                                     // flag = NS_BIND_REPLACE

        for (uint32_t i = 0; i < NS_BINDS_MAX; i++) {                                   // for each bind
            if (ns->binds[i].active && strcmp(ns->binds[i].new_path, new_path) == 0) {  // if path matches
                vnode_unref(ns->binds[i].vnode);                                        // unref vnode
                ns->binds[i].active = 0;
                ns->binds[i].vnode  = 0;
                ns->nbinds--;                                                           // deactivate entry
            }
        }
    }
 
    for (uint32_t i = 0; i < NS_BINDS_MAX; i++) {                                       // find a free slot
 
        if (ns->binds[i].active) continue;                                              // if entry inactive
 
        strncpy(ns->binds[i].new_path, new_path, VFS_PATH_MAX - 1);                     // use inactive entry
        ns->binds[i].new_path[VFS_PATH_MAX - 1] = '\0';
 
        // initialise entry
        ns->binds[i].vnode  = vnode;
        ns->binds[i].flags  = flags;
        ns->binds[i].active = 1;
        ns->binds[i].srv_fd = -1;                                                       // local vnode; 9P sets this
 
        vnode_ref(vnode);                                                               // reference vnode
        ns->nbinds++;                                                                   // update count
 
        kprintf("NS: bind \"%s\" flags=%u (ns=0x%p, total=%u)\n", new_path, (uint32_t)flags, (uint32_t)ns, ns->nbinds);
        return 0;
    }
    kprintf("NS: ns_bind - bind table full (max %u)\n", (uint32_t)NS_BINDS_MAX);
    return -1;
}

// unbind operation
int ns_unbind(ns_t **nsp, const char *new_path) {

    if (!nsp || !*nsp || !new_path) return -1;                                          // validate inputs
 
    if (ns_cow(nsp) < 0) return -1;                                                     // copy on bind
    ns_t *ns = *nsp;
 
    int removed = 0;
 
    for (uint32_t i = 0; i < NS_BINDS_MAX; i++) {                                       // iterate table
        if (!ns->binds[i].active) continue;
        if (strcmp(ns->binds[i].new_path, new_path) != 0) continue;                     // if path matches
 
        vnode_unref(ns->binds[i].vnode);                                                // unref vnode
        ns->binds[i].active = 0;                                                        // active = 0
        ns->binds[i].vnode  = 0;
        ns->nbinds--;                                                                   // update count
        removed++;
    }
 
    if (removed) {
        kprintf("NS: unbind \"%s\" (%d entries removed)\n", new_path, removed);
        return 0;
    }

    kprintf("NS: unbind \"%s\" - not found\n", new_path);
    return -1;
}

// find matching bind points
static uint32_t find_binds(const ns_t *ns, const char *path, const ns_bind_entry_t **matches, uint32_t max_matches, uint32_t *nmatches) {

    uint32_t best_len = 0;                                          // init search space
    *nmatches = 0;

    for (uint32_t i = 0; i < NS_BINDS_MAX; i++) {                   // for each bind entry

        if (!ns->binds[i].active) continue;                         // skip unused

        const char *np   = ns->binds[i].new_path;                   // return bind path
        uint32_t    nlen = (uint32_t)strlen(np);

        if (strncmp(np, path, nlen) != 0) continue;                 // prefix check

        // boundry check
        int boundary = (nlen == 1 && np[0] == '/') || (path[nlen] == '/') || (path[nlen] == '\0');
        if (!boundary) continue;

        if (nlen > best_len) {                                      // longest prefix logic
            best_len  = nlen;
            *nmatches = 0;                                          // new best: reset set
        }

        if (nlen == best_len && *nmatches < max_matches)            // if equal lengths,
            matches[(*nmatches)++] = &ns->binds[i];                 // store bind entry pointer
    }
    return best_len;                                                // return value
}

// walk remaining path from current vnode
static vnode_t *walk_path(vnode_t *cur, const char *rest) {

    char component[VFS_NAME_MAX];

    // extract next component
    while (*rest) {
 
        uint32_t n = 0;
        while (*rest && *rest != '/' && n < VFS_NAME_MAX - 1)                   // extract next component
            component[n++] = *rest++;
        component[n] = '\0';
        while (*rest == '/') rest++;                                            // skip slashes
 
        if (component[0] == '\0') continue;
 
        if (!cur->ops || !cur->ops->lookup) return 0;                           // verify lookup operating (directories only, no files)
 
        vnode_t *next = 0;
        if (cur->ops->lookup(cur, component, &next) < 0 || !next) return 0;     // call filesystem lookup
        cur = next;                                                             // continue walking
    }
    return cur;
}

// path resolution: using namespace binds + VFS
vnode_t *ns_resolve(ns_t *ns, const char *path) {

    if (!path || path[0] != '/') return 0;                                      // validate path start

    if (!ns || ns->nbinds == 0) return vfs_resolve(path);                       // no namespace: fall straight through to global VFS

    const ns_bind_entry_t *matches[NS_BINDS_MAX];
    uint32_t nmatches = 0;
    uint32_t best_len = find_binds(ns, path, matches, NS_BINDS_MAX, &nmatches); // find matching binds

    if (nmatches == 0) {                                                        // if no namespace match: use global VFS
        return vfs_resolve(path);
    }

    const char *rest = path + best_len;                                         // consume prefix: rest = remaining path components
    while (*rest == '/') rest++;

    if (*rest == '\0') {                                                        // bind match
        for (uint32_t i = 0; i < nmatches; i++) {
            if (matches[i]->flags == NS_BIND_BEFORE ||                          // BEFORE: highest priority
                matches[i]->flags == NS_BIND_REPLACE)                           // REPLACE: canonical
                return matches[i]->vnode;
        }
        return nmatches ? matches[0]->vnode : 0;                                // AFTER: fallback
    }

    // path continues past bind point: walk from each match in priority
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < nmatches; i++) {
            uint8_t f = matches[i]->flags;
            if (pass == 0 && f == NS_BIND_AFTER)    continue;                   // defer AFTER
            if (pass == 1 && f != NS_BIND_AFTER)    continue;                   // only AFTER
 
            vnode_t *result = walk_path(matches[i]->vnode, rest);
            if (result) return result;
        }
    }
    return vfs_resolve(path);                                                   // nothing in bind table resolved: global VFS fallback
}

#define NS_UNION_MAX 128                        // max merged entries across all union layers

// merge union directory listings
int ns_readdir(ns_t *ns, const char *dir_path, uint32_t index, char *name_out, uint32_t name_max, vnode_t **node_out) {

    if (!dir_path || !name_out || name_max == 0) return -1;

    // collect all vnodes at dir_path in priority order:
    vnode_t *layers[NS_BINDS_MAX + 1];
    uint32_t nlayers = 0;

    const ns_bind_entry_t *matches[NS_BINDS_MAX];
    uint32_t nmatches = 0;

    if (ns && ns->nbinds > 0)
        find_binds(ns, dir_path, matches, NS_BINDS_MAX, &nmatches);
 
    for (uint32_t i = 0; i < nmatches; i++) {                                           // pass 1: BEFORE
        if (matches[i]->flags == NS_BIND_BEFORE && nlayers < NS_BINDS_MAX)
            layers[nlayers++] = matches[i]->vnode;
    }

    uint8_t have_replace = 0;
    for (uint32_t i = 0; i < nmatches; i++) {                                           // REPLACE or global VFS
        if (matches[i]->flags == NS_BIND_REPLACE) {
            if (nlayers < NS_BINDS_MAX) layers[nlayers++] = matches[i]->vnode;
            have_replace = 1;
        }
    }
    if (!have_replace) {                                                                // try the global VFS at this path
        vnode_t *gv = vfs_resolve(dir_path);
        if (gv && gv->type == VNODE_DIR && nlayers < NS_BINDS_MAX)
            layers[nlayers++] = gv;
    }

    for (uint32_t i = 0; i < nmatches; i++) {                                           // pass 2: AFTER
        if (matches[i]->flags == NS_BIND_AFTER && nlayers < NS_BINDS_MAX)
            layers[nlayers++] = matches[i]->vnode;
    }

    if (nlayers == 0) return -1;

    // iterate merged entries, deduplicating by name, until we hit `index`
    char   seen[NS_UNION_MAX][VFS_NAME_MAX];
    uint32_t nseen  = 0;
    uint32_t logical = 0;

    for (uint32_t li = 0; li < nlayers; li++) {

        vnode_t *v = layers[li];
        if (!v || !v->ops || !v->ops->readdir) continue;

        uint32_t raw = 0;
        while (1) {
            char     entry_name[VFS_NAME_MAX];
            vnode_t *entry_node = 0;

            if (v->ops->readdir(v, raw, entry_name, &entry_node) < 0) break;
            raw++;

            uint8_t dup = 0;                                                            // dedup check
            for (uint32_t s = 0; s < nseen; s++) {
                if (strcmp(seen[s], entry_name) == 0) { dup = 1; break; }
            }
            if (dup) continue;

            if (nseen < NS_UNION_MAX)
                strncpy(seen[nseen++], entry_name, VFS_NAME_MAX - 1);
 
            if (logical == index) {
                strncpy(name_out, entry_name, name_max - 1);
                name_out[name_max - 1] = '\0';
                if (node_out) *node_out = entry_node;
                return 0;
            }
            logical++;
        }
    }
    return -1;                      // index beyond end of merged directory
}
 
// debug
void ns_dump(const ns_t *ns) {

    if (!ns) {
        kprintf("NS: (null namespace - using global VFS)\n");
        return;
    }

    kprintf("NS: namespace @ 0x%p  refcount=%u  binds=%u\n",
            (uint32_t)ns, ns->refcount, ns->nbinds);

    for (uint32_t i = 0; i < NS_BINDS_MAX; i++) {
        const ns_bind_entry_t *e = &ns->binds[i];
        if (!e->active) continue;

        const char *flag_str =
            (e->flags == NS_BIND_BEFORE)  ? "before" :
            (e->flags == NS_BIND_AFTER)   ? "after"  : "replace";

        kprintf("  [%2u] %s -> vnode=0x%p  mode=%s%s\n",
                i, e->new_path, (uint32_t)e->vnode, flag_str,
                (e->srv_fd >= 0) ? " (9P)" : "");
    }
}