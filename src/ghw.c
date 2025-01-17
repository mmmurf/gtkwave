/*  GHDL Wavefile reader interface.
    Copyright (C) 2005-2014 Tristan Gingold and Tony Bybell

    GHDL is free software; you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation; either version 2, or (at your option) any later
    version.

    GHDL is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with GCC; see the file COPYING.  If not, write to the Free
    Software Foundation, 51 Franklin Street - Suite 500, Boston, MA
    02110-1335, USA.
*/

#include "globals.h"
#include <config.h>
#include "ghw.h"
#include "libghw.h"
#include "tree.h"

/************************ splay ************************/

/*
 * NOTE:
 * a GHW tree's "which" is not the same as a gtkwave "which"
 * in that gtkwave's points to the facs[] array and
 * GHW's functions as an alias handle.  The following
 * (now global) vars are used to resolve those differences...
 *
 * static GwNode **nxp;
 * static struct symbol *sym_head = NULL, *sym_curr = NULL;
 * static int sym_which = 0;
 */

/*
 * pointer splay
 */
typedef struct ghw_tree_node ghw_Tree;
struct ghw_tree_node
{
    ghw_Tree *left, *right;
    void *item;
    int val_old;
    struct symbol *sym;
};

long ghw_cmp_l(void *i, void *j)
{
    uintptr_t il = (uintptr_t)i, jl = (uintptr_t)j;
    return (il - jl);
}

static ghw_Tree *ghw_splay(void *i, ghw_Tree *t)
{
    /* Simple top down splay, not requiring i to be in the tree t.  */
    /* What it does is described above.                             */
    ghw_Tree N, *l, *r, *y;
    int dir;

    if (t == NULL)
        return t;
    N.left = N.right = NULL;
    l = r = &N;

    for (;;) {
        dir = ghw_cmp_l(i, t->item);
        if (dir < 0) {
            if (t->left == NULL)
                break;
            if (ghw_cmp_l(i, t->left->item) < 0) {
                y = t->left; /* rotate right */
                t->left = y->right;
                y->right = t;
                t = y;
                if (t->left == NULL)
                    break;
            }
            r->left = t; /* link right */
            r = t;
            t = t->left;
        } else if (dir > 0) {
            if (t->right == NULL)
                break;
            if (ghw_cmp_l(i, t->right->item) > 0) {
                y = t->right; /* rotate left */
                t->right = y->left;
                y->left = t;
                t = y;
                if (t->right == NULL)
                    break;
            }
            l->right = t; /* link left */
            l = t;
            t = t->right;
        } else {
            break;
        }
    }
    l->right = t->left; /* assemble */
    r->left = t->right;
    t->left = N.right;
    t->right = N.left;
    return t;
}

static ghw_Tree *ghw_insert(void *i, ghw_Tree *t, int val, struct symbol *sym)
{
    /* Insert i into the tree t, unless it's already there.    */
    /* Return a pointer to the resulting tree.                 */
    ghw_Tree *n;
    int dir;

    n = (ghw_Tree *)calloc_2(1, sizeof(ghw_Tree));
    if (n == NULL) {
        fprintf(stderr, "ghw_insert: ran out of memory, exiting.\n");
        exit(255);
    }
    n->item = i;
    n->val_old = val;
    n->sym = sym;
    if (t == NULL) {
        n->left = n->right = NULL;
        return n;
    }
    t = ghw_splay(i, t);
    dir = ghw_cmp_l(i, t->item);
    if (dir < 0) {
        n->left = t->left;
        n->right = t;
        t->left = NULL;
        return n;
    } else if (dir > 0) {
        n->right = t->right;
        n->left = t;
        t->right = NULL;
        return n;
    } else { /* We get here if it's already in the tree */
        /* Don't add it again                      */
        free_2(n);
        return t;
    }
}

/*
 * chain together bits of the same fac
 */
int strand_pnt(char *s)
{
    int len = strlen(s) - 1;
    int i;
    int rc = -1;

    if (s[len] == ']') {
        for (i = len - 1; i > 0; i--) {
            if (((s[i] >= '0') && (s[i] <= '9')) || (s[i] == '-'))
                continue;
            if (s[i] != '[')
                break;
            return (i); /* position right before left bracket for strncmp */
        }
    }

    return (rc);
}

void rechain_facs(void)
{
    int i;
    struct symbol *psr = NULL;
    struct symbol *root = NULL;

    for (i = 0; i < GLOBALS->numfacs; i++) {
        if (psr) {
            int ev1 = GLOBALS->facs[i - 1]->n->extvals;
            int ev2 = GLOBALS->facs[i]->n->extvals;

            if (!ev1 && !ev2) {
                char *fstr1 = GLOBALS->facs[i - 1]->name;
                char *fstr2 = GLOBALS->facs[i]->name;
                int p1 = strand_pnt(fstr1);
                int p2 = strand_pnt(fstr2);

                if (!root) {
                    if ((p1 >= 0) && (p1 == p2)) {
                        if (!strncmp(fstr1, fstr2, p1)) {
                            root = GLOBALS->facs[i - 1];
                            root->vec_root = root;
                            root->vec_chain = GLOBALS->facs[i];
                            GLOBALS->facs[i]->vec_root = root;
                        }
                    }
                } else {
                    if ((p1 >= 0) && (p1 == p2)) {
                        if (!strncmp(fstr1, fstr2, p1)) {
                            psr->vec_chain = GLOBALS->facs[i];
                            GLOBALS->facs[i]->vec_root = root;
                        } else {
                            root = NULL;
                        }
                    } else {
                        root = NULL;
                    }
                }
            } else {
                root = NULL;
            }
        }

        psr = GLOBALS->facs[i];
    }
}

/*
 * preserve tree->t_which ordering so hierarchy children index pointers don't get corrupted
 */

#if 1

/* limited recursive version */

static void recurse_tree_build_whichcache(GwTree *t)
{
    if (t) {
        GwTree *t2 = t;
        int i;
        int cnt = 1;
        GwTree **ar;

        while ((t2 = t2->next)) {
            cnt++;
        }

        ar = malloc_2(cnt * sizeof(GwTree *));
        t2 = t;
        for (i = 0; i < cnt; i++) {
            ar[i] = t2;
            if (t2->child) {
                recurse_tree_build_whichcache(t2->child);
            }
            t2 = t2->next;
        }

        for (i = cnt - 1; i >= 0; i--) {
            t = ar[i];
            if (t->t_which >= 0) {
                GLOBALS->gwt_ghw_c_1 =
                    ghw_insert(t, GLOBALS->gwt_ghw_c_1, t->t_which, GLOBALS->facs[t->t_which]);
            }
        }

        free_2(ar);
    }
}

static void recurse_tree_fix_from_whichcache(GwTree *t)
{
    if (t) {
        GwTree *t2 = t;
        int i;
        int cnt = 1;
        GwTree **ar;

        while ((t2 = t2->next)) {
            cnt++;
        }

        ar = malloc_2(cnt * sizeof(GwTree *));
        t2 = t;
        for (i = 0; i < cnt; i++) {
            ar[i] = t2;
            if (t2->child) {
                recurse_tree_fix_from_whichcache(t2->child);
            }
            t2 = t2->next;
        }

        for (i = cnt - 1; i >= 0; i--) {
            t = ar[i];
            if (t->t_which >= 0) {
                GLOBALS->gwt_ghw_c_1 = ghw_splay(t, GLOBALS->gwt_ghw_c_1);
                GLOBALS->gwt_corr_ghw_c_1 = ghw_splay(
                    GLOBALS->gwt_ghw_c_1->sym,
                    GLOBALS->gwt_corr_ghw_c_1); /* all facs are in this tree so this is OK */

                t->t_which = GLOBALS->gwt_corr_ghw_c_1->val_old;
            }
        }

        free_2(ar);
    }
}

#else

/* original fully-recursive version */

static void recurse_tree_build_whichcache(GwTree *t)
{
    if (t) {
        if (t->child) {
            recurse_tree_build_whichcache(t->child);
        }
        if (t->next) {
            recurse_tree_build_whichcache(t->next);
        }

        if (t->t_which >= 0)
            GLOBALS->gwt_ghw_c_1 =
                ghw_insert(t, GLOBALS->gwt_ghw_c_1, t->t_which, GLOBALS->facs[t->t_which]);
    }
}

static void recurse_tree_fix_from_whichcache(GwTree *t)
{
    if (t) {
        if (t->child) {
            recurse_tree_fix_from_whichcache(t->child);
        }
        if (t->next) {
            recurse_tree_fix_from_whichcache(t->next);
        }

        if (t->t_which >= 0) {
            GLOBALS->gwt_ghw_c_1 = ghw_splay(t, GLOBALS->gwt_ghw_c_1);
            GLOBALS->gwt_corr_ghw_c_1 =
                ghw_splay(GLOBALS->gwt_ghw_c_1->sym,
                          GLOBALS->gwt_corr_ghw_c_1); /* all facs are in this tree so this is OK */

            t->t_which = GLOBALS->gwt_corr_ghw_c_1->val_old;
        }
    }
}

#endif

static void incinerate_whichcache_tree(ghw_Tree *t)
{
    if (t->left)
        incinerate_whichcache_tree(t->left);
    if (t->right)
        incinerate_whichcache_tree(t->right);

    free_2(t);
}

/*
 * sort facs and also cache/reconGwTree->t_which pointers
 */
static void ghw_sortfacs(void)
{
    int i;

    recurse_tree_build_whichcache(GLOBALS->treeroot);

    for (i = 0; i < GLOBALS->numfacs; i++) {
        char *subst;
        int len;
        struct symbol *curnode = GLOBALS->facs[i];

        subst = curnode->name;
        if ((len = strlen(subst)) > GLOBALS->longestname)
            GLOBALS->longestname = len;
    }

    wave_heapsort(GLOBALS->facs, GLOBALS->numfacs);

    for (i = 0; i < GLOBALS->numfacs; i++) {
        GLOBALS->gwt_corr_ghw_c_1 =
            ghw_insert(GLOBALS->facs[i], GLOBALS->gwt_corr_ghw_c_1, i, NULL);
    }

    recurse_tree_fix_from_whichcache(GLOBALS->treeroot);
    if (GLOBALS->gwt_ghw_c_1) {
        incinerate_whichcache_tree(GLOBALS->gwt_ghw_c_1);
        GLOBALS->gwt_ghw_c_1 = NULL;
    }
    if (GLOBALS->gwt_corr_ghw_c_1) {
        incinerate_whichcache_tree(GLOBALS->gwt_corr_ghw_c_1);
        GLOBALS->gwt_corr_ghw_c_1 = NULL;
    }

    GLOBALS->facs_are_sorted = 1;
}

/*******************************************************************************/

static GwTree *build_hierarchy_type(struct ghw_handler *h,
                                         union ghw_type *t,
                                         const char *pfx,
                                         unsigned int **sig);

static GwTree *build_hierarchy_record(struct ghw_handler *h,
                                           const char *pfx,
                                           unsigned nbr_els,
                                           struct ghw_record_element *els,
                                           unsigned int **sig)
{
    GwTree *res;
    GwTree *last;
    GwTree *c;
    unsigned int i;

    res = (GwTree *)calloc_2(1, sizeof(GwTree) + strlen(pfx) + 1);
    strcpy(res->name, (char *)pfx);
    res->t_which = WAVE_T_WHICH_UNDEFINED_COMPNAME;

    last = NULL;
    for (i = 0; i < nbr_els; i++) {
        c = build_hierarchy_type(h, els[i].type, els[i].name, sig);
        if (last == NULL)
            res->child = c;
        else
            last->next = c;
        last = c;
    }
    return res;
}

static void build_hierarchy_array(struct ghw_handler *h,
                                  union ghw_type *arr,
                                  int dim,
                                  const char *pfx,
                                  GwTree **res,
                                  unsigned int **sig)
{
    union ghw_type *idx;
    struct ghw_type_array *base = (struct ghw_type_array *)ghw_get_base_type(arr->sa.base);
    char *name = NULL;

    if ((unsigned int)dim == base->nbr_dim) {
        GwTree *t;
        sprintf(GLOBALS->asbuf, "%s]", pfx);
        name = strdup_2(GLOBALS->asbuf);

        t = build_hierarchy_type(h, arr->sa.el, name, sig);

        if (*res != NULL)
            (*res)->next = t;
        *res = t;
        return;
    }

    idx = ghw_get_base_type(base->dims[dim]);
    switch (idx->kind) {
        case ghdl_rtik_type_i32: {
            int32_t v;
            char *nam;
            struct ghw_range_i32 *r;
            /* GwTree *last; */
            int len;

            /* last = NULL; */
            r = &arr->sa.rngs[dim]->i32;
            len = ghw_get_range_length((union ghw_range *)r);
            if (len <= 0)
                break;
            v = r->left;
            while (1) {
                sprintf(GLOBALS->asbuf, "%s%c" GHWPRI32, pfx, dim == 0 ? '[' : ',', v);
                nam = strdup_2(GLOBALS->asbuf);
                build_hierarchy_array(h, arr, dim + 1, nam, res, sig);
                free_2(nam);
                if (v == r->right)
                    break;
                if (r->dir == 0)
                    v++;
                else
                    v--;
            }
        }
            return;

        case ghdl_rtik_type_e8: {
            int32_t v;
            char *nam;
            struct ghw_range_e8 *r;
            /* GwTree *last; */
            int len;

            /* last = NULL; */
            r = &arr->sa.rngs[dim]->e8;
            len = ghw_get_range_length((union ghw_range *)r);
            if (len <= 0)
                break;
            v = r->left;
            while (1) {
                sprintf(GLOBALS->asbuf, "%s%c" GHWPRI32, pfx, dim == 0 ? '[' : ',', v);
                nam = strdup_2(GLOBALS->asbuf);
                build_hierarchy_array(h, arr, dim + 1, nam, res, sig);
                free_2(nam);
                if (v == r->right)
                    break;
                if (r->dir == 0)
                    v++;
                else
                    v--;
            }
        }
            return;
        /* PATCH-BEGIN: */
        case ghdl_rtik_type_b2: {
            int32_t v;
            char *nam;
            struct ghw_range_b2 *r;
            /* GwTree *last; */
            int len;

            /* last = NULL; */
            r = &arr->sa.rngs[dim]->b2;
            len = ghw_get_range_length((union ghw_range *)r);
            if (len <= 0)
                break;
            v = r->left;
            while (1) {
                sprintf(GLOBALS->asbuf, "%s%c" GHWPRI32, pfx, dim == 0 ? '[' : ',', v);
                nam = strdup_2(GLOBALS->asbuf);
                build_hierarchy_array(h, arr, dim + 1, nam, res, sig);
                free_2(nam);
                if (v == r->right)
                    break;
                if (r->dir == 0)
                    v++;
                else
                    v--;
            }
        }
            return;
            /* PATCH-END: */
        default:
            fprintf(stderr, "build_hierarchy_array: unhandled type %d\n", idx->kind);
            abort();
    }
}

static GwTree *build_hierarchy_type(struct ghw_handler *h,
                                         union ghw_type *t,
                                         const char *pfx,
                                         unsigned int **sig)
{
    GwTree *res;
    struct symbol *s;

    switch (t->kind) {
        case ghdl_rtik_subtype_scalar:
            return build_hierarchy_type(h, t->ss.base, pfx, sig);
        case ghdl_rtik_type_b2:
        case ghdl_rtik_type_e8:
        case ghdl_rtik_type_f64:
        case ghdl_rtik_type_i32:
        case ghdl_rtik_type_i64:
        case ghdl_rtik_type_p32:
        case ghdl_rtik_type_p64:

            s = calloc_2(1, sizeof(struct symbol));

            if (!GLOBALS->firstnode) {
                GLOBALS->firstnode = GLOBALS->curnode = calloc_2(1, sizeof(struct symchain));
            } else {
                GLOBALS->curnode->next = calloc_2(1, sizeof(struct symchain));
                GLOBALS->curnode = GLOBALS->curnode->next;
            }
            GLOBALS->curnode->symbol = s;

            GLOBALS->nbr_sig_ref_ghw_c_1++;
            res = (GwTree *)calloc_2(1, sizeof(GwTree) + strlen(pfx) + 1);
            strcpy(res->name, (char *)pfx);
            res->t_which = *(*sig)++;

            s->n = GLOBALS->nxp_ghw_c_1[res->t_which];
            return res;
        case ghdl_rtik_subtype_array:
        case ghdl_rtik_subtype_array_ptr: {
            GwTree *r;
            res = (GwTree *)calloc_2(1, sizeof(GwTree) + strlen(pfx) + 1);
            strcpy(res->name, (char *)pfx);
            res->t_which = WAVE_T_WHICH_UNDEFINED_COMPNAME;
            r = res;
            build_hierarchy_array(h, t, 0, "", &res, sig);
            r->child = r->next;
            r->next = NULL;
            return r;
        }
        case ghdl_rtik_type_record:
            return build_hierarchy_record(h, pfx, t->rec.nbr_fields, t->rec.els, sig);
        case ghdl_rtik_subtype_record:
            return build_hierarchy_record(h, pfx, t->sr.base->nbr_fields, t->sr.els, sig);
        default:
            fprintf(stderr, "build_hierarchy_type: unhandled type %d\n", t->kind);
            abort();
    }
}

/* Create the gtkwave tree from the GHW hierarchy.  */

static GwTree *build_hierarchy(struct ghw_handler *h, struct ghw_hie *hie)
{
    GwTree *t;
    GwTree *t_ch;
    GwTree *prev;
    struct ghw_hie *ch;
    unsigned char ttype;

    switch (hie->kind) {
        case ghw_hie_design:
        case ghw_hie_block:
        case ghw_hie_instance:
        case ghw_hie_generate_for:
        case ghw_hie_generate_if:
        case ghw_hie_package:

            /* Convert kind.  */
            switch (hie->kind) {
                case ghw_hie_design:
                    ttype = GW_TREE_KIND_VHDL_ST_DESIGN;
                    break;
                case ghw_hie_block:
                    ttype = GW_TREE_KIND_VHDL_ST_BLOCK;
                    break;
                case ghw_hie_instance:
                    ttype = GW_TREE_KIND_VHDL_ST_INSTANCE;
                    break;
                case ghw_hie_generate_for:
                    ttype = GW_TREE_KIND_VHDL_ST_GENFOR;
                    break;
                case ghw_hie_generate_if:
                    ttype = GW_TREE_KIND_VHDL_ST_GENIF;
                    break;
                case ghw_hie_package:
                default:
                    ttype = GW_TREE_KIND_VHDL_ST_PACKAGE;
                    break;
            }

            /* For iterative generate, add the index.  */
            if (hie->kind == ghw_hie_generate_for) {
                char buf[128];
                int name_len, buf_len;
                char *n;

                ghw_get_value(buf, sizeof(buf), hie->u.blk.iter_value, hie->u.blk.iter_type);
                name_len = strlen(hie->name);
                buf_len = strlen(buf);

                t = (GwTree *)calloc_2(1, sizeof(GwTree) + (2 + buf_len + name_len + 1));
                t->kind = ttype;
                n = t->name;

                memcpy(n, hie->name, name_len);
                n += name_len;
                *n++ = '[';
                memcpy(n, buf, buf_len);
                n += buf_len;
                *n++ = ']';
                *n = 0;
            } else {
                if (hie->name) {
                    t = (GwTree *)calloc_2(1, sizeof(GwTree) + strlen(hie->name) + 1);
                    t->kind = ttype;
                    strcpy(t->name, (char *)hie->name);
                } else {
                    t = (GwTree *)calloc_2(1, sizeof(GwTree) + 1);
                    t->kind = ttype;
                }
            }

            t->t_which = WAVE_T_WHICH_UNDEFINED_COMPNAME;

            /* Recurse.  */
            prev = NULL;
            for (ch = hie->u.blk.child; ch != NULL; ch = ch->brother) {
                t_ch = build_hierarchy(h, ch);
                if (t_ch != NULL) {
                    if (prev == NULL)
                        t->child = t_ch;
                    else
                        prev->next = t_ch;
                    prev = t_ch;
                }
            }
            return t;
        case ghw_hie_process:
            return NULL;
        case ghw_hie_signal:
        case ghw_hie_port_in:
        case ghw_hie_port_out:
        case ghw_hie_port_inout:
        case ghw_hie_port_buffer:
        case ghw_hie_port_linkage: {
            unsigned int *ptr = hie->u.sig.sigs;

            /* Convert kind.  */
            switch (hie->kind) {
                case ghw_hie_signal:
                    ttype = GW_TREE_KIND_VHDL_ST_SIGNAL;
                    break;
                case ghw_hie_port_in:
                    ttype = GW_TREE_KIND_VHDL_ST_PORTIN;
                    break;
                case ghw_hie_port_out:
                    ttype = GW_TREE_KIND_VHDL_ST_PORTOUT;
                    break;
                case ghw_hie_port_inout:
                    ttype = GW_TREE_KIND_VHDL_ST_PORTINOUT;
                    break;
                case ghw_hie_port_buffer:
                    ttype = GW_TREE_KIND_VHDL_ST_BUFFER;
                    break;
                case ghw_hie_port_linkage:
                default:
                    ttype = GW_TREE_KIND_VHDL_ST_LINKAGE;
                    break;
            }

            /* Convert type.  */
            t = build_hierarchy_type(h, hie->u.sig.type, hie->name, &ptr);
            if (*ptr != 0)
                abort();
            if (t) {
                t->kind = ttype;
            }
            return t;
        }
        default:
            fprintf(stderr, "ghw: build_hierarchy: cannot handle hie %d\n", hie->kind);
            abort();
    }
}

void facs_debug(void)
{
    int i;
    GwNode *n;

    for (i = 0; i < GLOBALS->numfacs; i++) {
        GwHistEnt *h;

        n = GLOBALS->facs[i]->n;
        printf("%d: %s\n", i, n->nname);
        if (n->extvals)
            printf("  ext: %d - %d\n", n->msi, n->lsi);
        for (h = &n->head; h; h = h->next)
            printf("  time:%" GW_TIME_FORMAT " flags:%02x vect:%p\n",
                   h->time,
                   h->flags,
                   h->v.h_vector);
    }
}

static void create_facs(struct ghw_handler *h)
{
    unsigned int i;
    struct symchain *sc = GLOBALS->firstnode;

    GLOBALS->numfacs = GLOBALS->nbr_sig_ref_ghw_c_1;
    GLOBALS->facs = (struct symbol **)malloc_2(GLOBALS->numfacs * sizeof(struct symbol *));

    i = 0;
    while (sc) {
        GLOBALS->facs[i++] = sc->symbol;
        sc = sc->next;
    }

    for (i = 0; i < h->nbr_sigs; i++) {
        GwNode *n = GLOBALS->nxp_ghw_c_1[i];

        if (h->sigs[i].type)
            switch (h->sigs[i].type->kind) {
                case ghdl_rtik_type_b2:
                    if (h->sigs[i].type->en.wkt == ghw_wkt_bit) {
                        n->extvals = 0;
                        break;
                    }
                    /* FALLTHROUGH */
                case ghdl_rtik_type_e8:
                    if (GLOBALS->xlat_1164_ghw_c_1 &&
                        h->sigs[i].type->en.wkt == ghw_wkt_std_ulogic) {
                        n->extvals = 0;
                        break;
                    }
                    /* FALLTHROUGH */
                case ghdl_rtik_type_i32:
                case ghdl_rtik_type_p32:
                    n->extvals = 1;
                    n->msi = 31;
                    n->lsi = 0;
                    n->vartype = GW_VAR_TYPE_VCD_INTEGER;
                    break;
                case ghdl_rtik_type_i64:
                case ghdl_rtik_type_p64:
                    n->extvals = 1;
                    n->msi = 63;
                    n->lsi = 0;
                    n->vartype = GW_VAR_TYPE_VCD_INTEGER;
                    break;
                case ghdl_rtik_type_e32: /* ajb: what is e32? */
                case ghdl_rtik_type_f64:
                    n->extvals = 1;
                    n->msi = n->lsi = 0;
                    break;
                default:
                    fprintf(stderr, "ghw:create_facs: unhandled kind %d\n", h->sigs[i].type->kind);
                    n->extvals = 0;
            }
    }
}

static void set_fac_name_1(GwTree *t)
{
    for (; t != NULL; t = t->next) {
        int prev_len = GLOBALS->fac_name_len_ghw_c_1;

        /* Complete the name.  */
        if (t->name[0]) /* originally (t->name != NULL) when using pointers */
        {
            int len;

            len = strlen(t->name) + 1;
            if (len + GLOBALS->fac_name_len_ghw_c_1 >= GLOBALS->fac_name_max_ghw_c_1) {
                GLOBALS->fac_name_max_ghw_c_1 *= 2;
                if (GLOBALS->fac_name_max_ghw_c_1 <= len + GLOBALS->fac_name_len_ghw_c_1)
                    GLOBALS->fac_name_max_ghw_c_1 = len + GLOBALS->fac_name_len_ghw_c_1 + 1;
                GLOBALS->fac_name_ghw_c_1 =
                    realloc_2(GLOBALS->fac_name_ghw_c_1, GLOBALS->fac_name_max_ghw_c_1);
            }
            if (t->name[0] != '[') {
                GLOBALS->fac_name_ghw_c_1[GLOBALS->fac_name_len_ghw_c_1] = '.';
                /* The NUL is copied, since LEN is 1 + strlen.  */
                memcpy(GLOBALS->fac_name_ghw_c_1 + GLOBALS->fac_name_len_ghw_c_1 + 1, t->name, len);
                GLOBALS->fac_name_len_ghw_c_1 += len;
            } else {
                memcpy(GLOBALS->fac_name_ghw_c_1 + GLOBALS->fac_name_len_ghw_c_1, t->name, len);
                GLOBALS->fac_name_len_ghw_c_1 += (len - 1);
            }
        }

        if (t->t_which >= 0) {
            struct symchain *sc = GLOBALS->firstnode;
            struct symbol *s = sc->symbol;

            s->name = strdup_2(GLOBALS->fac_name_ghw_c_1);
            s->n = GLOBALS->nxp_ghw_c_1[t->t_which];
            if (!s->n->nname)
                s->n->nname = s->name;

            t->t_which =
                GLOBALS->sym_which_ghw_c_1++; /* patch in gtkwave "which" as node is correct */

            GLOBALS->curnode = GLOBALS->firstnode->next;
            free_2(GLOBALS->firstnode);
            GLOBALS->firstnode = GLOBALS->curnode;
        }

        if (t->child)
            set_fac_name_1(t->child);

        /* Revert name.  */
        GLOBALS->fac_name_len_ghw_c_1 = prev_len;
        GLOBALS->fac_name_ghw_c_1[GLOBALS->fac_name_len_ghw_c_1] = 0;
    }
}

static void set_fac_name(struct ghw_handler *h)
{
    if (GLOBALS->fac_name_max_ghw_c_1 == 0) {
        GLOBALS->fac_name_max_ghw_c_1 = 1024;
        GLOBALS->fac_name_ghw_c_1 = malloc_2(GLOBALS->fac_name_max_ghw_c_1);
    }
    GLOBALS->fac_name_len_ghw_c_1 = 3;
    memcpy(GLOBALS->fac_name_ghw_c_1, "top", 4);
    GLOBALS->last_fac_ghw_c_1 = h->nbr_sigs;
    set_fac_name_1(GLOBALS->treeroot);
}

static void add_history(struct ghw_handler *h, GwNode *n, int sig_num)
{
    GwHistEnt *he;
    struct ghw_sig *sig = &h->sigs[sig_num];
    union ghw_type *sig_type = sig->type;
    int flags;
    int is_vector = 0;
    int is_double = 0;

    if (sig_type == NULL)
        return;

    GLOBALS->regions++;

    switch (sig_type->kind) {
        case ghdl_rtik_type_i32:
        case ghdl_rtik_type_i64:
        case ghdl_rtik_type_p32:
        case ghdl_rtik_type_p64:
            flags = 0;
            break;

        case ghdl_rtik_type_b2:
            if (sig_type->en.wkt == ghw_wkt_bit) {
                flags = 0;
                break;
            }
            /* FALLTHROUGH */
        case ghdl_rtik_type_e8:
            if (GLOBALS->xlat_1164_ghw_c_1 && sig_type->en.wkt == ghw_wkt_std_ulogic) {
                flags = 0;
                break;
            }
            /* FALLTHROUGH */
        case ghdl_rtik_type_e32:
            flags = GW_HIST_ENT_FLAG_STRING | GW_HIST_ENT_FLAG_REAL;
            if (GW_HIST_ENT_FLAG_STRING == 0) {
                if (!GLOBALS->warned_ghw_c_1)
                    fprintf(stderr, "warning: do not compile with STRICT_VCD\n");
                GLOBALS->warned_ghw_c_1 = 1;
                return;
            }
            break;
        case ghdl_rtik_type_f64:
            flags = GW_HIST_ENT_FLAG_REAL;
            break;
        default:
            fprintf(stderr, "ghw:add_history: unhandled kind %d\n", sig->type->kind);
            return;
    }

    if (!n->curr) {
        he = histent_calloc();
        he->flags = flags;
        he->time = -1;
        he->v.h_vector = NULL;

        n->head.next = he;
        n->curr = he;
        n->head.time = -2;
    }

    he = histent_calloc();
    he->flags = flags;
    he->time = h->snap_time;

    switch (sig_type->kind) {
        case ghdl_rtik_type_b2:
            if (sig_type->en.wkt == ghw_wkt_bit)
                he->v.h_val = sig->val->b2 == 0 ? AN_0 : AN_1;
            else {
                he->v.h_vector = (char *)sig->type->en.lits[sig->val->b2];
                is_vector = 1;
            }
            break;
        case ghdl_rtik_type_e8:
            if (GLOBALS->xlat_1164_ghw_c_1 && sig_type->en.wkt == ghw_wkt_std_ulogic) {
                /* Res: 0->0, 1->X, 2->Z, 3->1 */
                static const char map_su2vlg[9] = {/* U */ AN_U,
                                                   /* X */ AN_X,
                                                   /* 0 */ AN_0,
                                                   /* 1 */ AN_1,
                                                   /* Z */ AN_Z,
                                                   /* W */ AN_W,
                                                   /* L */ AN_L,
                                                   /* H */ AN_H,
                                                   /* - */ AN_DASH};
                he->v.h_val = map_su2vlg[sig->val->e8];
            } else {
                he->v.h_vector = (char *)sig_type->en.lits[sig->val->e8];
                is_vector = 1;
            }
            break;
        case ghdl_rtik_type_f64: {
            he->v.h_double = sig->val->f64;
            is_double = 1;
        } break;
        case ghdl_rtik_type_i32:
        case ghdl_rtik_type_p32: {
            int i;
            he->v.h_vector = malloc_2(32);
            for (i = 0; i < 32; i++) {
                he->v.h_vector[31 - i] = ((sig->val->i32 >> i) & 1) ? AN_1 : AN_0;
            }
        }
            is_vector = 1;
            break;
        case ghdl_rtik_type_i64:
        case ghdl_rtik_type_p64: {
            int i;
            he->v.h_vector = malloc_2(64);
            for (i = 0; i < 64; i++) {
                he->v.h_vector[63 - i] = ((sig->val->i64 >> i) & 1) ? AN_1 : AN_0;
            }
        }
            is_vector = 1;
            break;
        default:
            abort();
    }

    /* deglitch */
    if (n->curr->time == he->time) {
        int gl_add = 0;

        if (n->curr->time) /* filter out time zero glitches */
        {
            gl_add = 1;
        }

        GLOBALS->num_glitches_ghw_c_1 += gl_add;

        if (!(n->curr->flags & GW_HIST_ENT_FLAG_GLITCH)) {
            if (gl_add) {
                n->curr->flags |= GW_HIST_ENT_FLAG_GLITCH; /* set the glitch flag */
                GLOBALS->num_glitch_regions_ghw_c_1++;
            }
        }

        if (is_double) {
            n->curr->v.h_double = he->v.h_double;
        } else if (is_vector) {
            if (n->curr->v.h_vector && sig_type->kind != ghdl_rtik_type_b2 &&
                sig_type->kind != ghdl_rtik_type_e8)
                free_2(n->curr->v.h_vector);
            n->curr->v.h_vector = he->v.h_vector;
            /* can't free up this "he" because of block allocation so assume it's dead */
        } else {
            n->curr->v.h_val = he->v.h_val;
        }
        return;
    } else /* look for duplicate dumps of same value at adjacent times */
    {
        if (!is_vector & !is_double) {
            if (n->curr->v.h_val == he->v.h_val) {
                return;
                /* can't free up this "he" because of block allocation so assume it's dead */
            }
        }
    }

    n->curr->next = he;
    n->curr = he;
}

static void add_tail(struct ghw_handler *h)
{
    unsigned int i;
    GwTime j;

    for (j = 1; j >= 0; j--) /* add two endcaps */
        for (i = 0; i < h->nbr_sigs; i++) {
            struct ghw_sig *sig = &h->sigs[i];
            GwNode *n = GLOBALS->nxp_ghw_c_1[i];
            GwHistEnt *he;

            if (sig->type == NULL || n == NULL || !n->curr)
                continue;

            /* Copy the last one.  */
            he = histent_calloc();
            *he = *n->curr;
            he->time = MAX_HISTENT_TIME - j;
            he->next = NULL;

            /* Append.  */
            n->curr->next = he;
            n->curr = he;
        }
}

static void read_traces(struct ghw_handler *h)
{
    int *list;
    unsigned int i;
    enum ghw_res res;

    list = malloc_2((GLOBALS->numfacs + 1) * sizeof(int));

    while (1) {
        res = ghw_read_sm_hdr(h, list);
        switch (res) {
            case ghw_res_error:
            case ghw_res_eof:
                free_2(list);
                return;
            case ghw_res_ok:
            case ghw_res_other:
                break;
            case ghw_res_snapshot:
                if (h->snap_time > GLOBALS->max_time)
                    GLOBALS->max_time = h->snap_time;
                /* printf ("Time is "GHWPRI64"\n", h->snap_time); */

                for (i = 0; i < h->nbr_sigs; i++)
                    add_history(h, GLOBALS->nxp_ghw_c_1[i], i);
                break;
            case ghw_res_cycle:
                while (1) {
                    int sig;

                    /* printf ("Time is "GHWPRI64"\n", h->snap_time); */
                    if (h->snap_time < GW_TIME_CONSTANT(9223372036854775807)) {
                        if (h->snap_time > GLOBALS->max_time)
                            GLOBALS->max_time = h->snap_time;

                        for (i = 0; (sig = list[i]) != 0; i++)
                            add_history(h, GLOBALS->nxp_ghw_c_1[sig], sig);
                    }
                    res = ghw_read_cycle_next(h);
                    if (res != 1)
                        break;
                    res = ghw_read_cycle_cont(h, list);
                    if (res < 0)
                        break;
                }
                if (res < 0)
                    break;
                res = ghw_read_cycle_end(h);
                if (res < 0)
                    break;
                break;
            default:
                break;
        }
    }
}

/*******************************************************************************/

GwTime ghw_main(char *fname)
{
    struct ghw_handler handle;
    int i;
    unsigned int ui;
    int rc;

    if (!GLOBALS->hier_was_explicitly_set) /* set default hierarchy split char */
    {
        GLOBALS->hier_delimeter = '.';
    }

    handle.flag_verbose = 0;
    if ((rc = ghw_open(&handle, fname)) < 0) {
        fprintf(stderr, "Error opening ghw file '%s', rc=%d.\n", fname, rc);
        return (GW_TIME_CONSTANT(0)); /* look at return code in caller for success status... */
    }

    GLOBALS->time_scale = 1;
    GLOBALS->time_dimension = 'f';
    GLOBALS->asbuf = malloc_2(4097);

    if (ghw_read_base(&handle) < 0) {
        free_2(GLOBALS->asbuf);
        GLOBALS->asbuf = NULL;
        fprintf(stderr, "Error in ghw file '%s'.\n", fname);
        return (GW_TIME_CONSTANT(0)); /* look at return code in caller for success status... */
    }

    GLOBALS->min_time = 0;
    GLOBALS->max_time = 0;

    GLOBALS->nbr_sig_ref_ghw_c_1 = 0;

    GLOBALS->nxp_ghw_c_1 = calloc_2(handle.nbr_sigs, sizeof(GwNode *));
    for (ui = 0; ui < handle.nbr_sigs; ui++) {
        GLOBALS->nxp_ghw_c_1[ui] = calloc_2(1, sizeof(GwNode));
    }

    GLOBALS->treeroot = build_hierarchy(&handle, handle.hie);
    /* GHW does not contains a 'top' name.
       FIXME: should use basename of the file.  */

    create_facs(&handle);
    read_traces(&handle);
    add_tail(&handle);

    set_fac_name(&handle);
    free_2(GLOBALS->nxp_ghw_c_1);
    GLOBALS->nxp_ghw_c_1 = NULL;

    /* fix up names on aliased nodes via cloning... */
    for (i = 0; i < GLOBALS->numfacs; i++) {
        if (strcmp(GLOBALS->facs[i]->name, GLOBALS->facs[i]->n->nname)) {
            GwNode *n = malloc_2(sizeof(GwNode));
            memcpy(n, GLOBALS->facs[i]->n, sizeof(GwNode));
            GLOBALS->facs[i]->n = n;
            n->nname = GLOBALS->facs[i]->name;
        }
    }

    /* treeroot->name = "top"; */
    {
        const char *base_hier = "top";

        GwTree *t = calloc_2(1, sizeof(GwTree) + strlen(base_hier) + 1);
        memcpy(t, GLOBALS->treeroot, sizeof(GwTree));
        strcpy(t->name,
               base_hier); /* scan-build false warning here, thinks name[1] is total length */
#ifndef WAVE_TALLOC_POOL_SIZE
        free_2(GLOBALS->treeroot); /* if using tree alloc pool, can't deallocate this */
#endif
        GLOBALS->treeroot = t;
    }

    ghw_close(&handle);

    rechain_facs(); /* vectorize bitblasted nets */
    ghw_sortfacs(); /* sort nets as ghw is unsorted ... also fix hier tree (it should really be
                       built *after* facs are sorted!) */

#if 0
 treedebug(GLOBALS->treeroot,"");
 facs_debug();
#endif

    GLOBALS->is_ghw = 1;

    fprintf(stderr,
            "[%" GW_TIME_FORMAT "] start time.\n[%" GW_TIME_FORMAT "] end time.\n",
            GLOBALS->min_time * GLOBALS->time_scale,
            GLOBALS->max_time * GLOBALS->time_scale);
    if (GLOBALS->num_glitches_ghw_c_1)
        fprintf(stderr,
                "Warning: encountered %d glitch%s across %d glitch region%s.\n",
                GLOBALS->num_glitches_ghw_c_1,
                (GLOBALS->num_glitches_ghw_c_1 != 1) ? "es" : "",
                GLOBALS->num_glitch_regions_ghw_c_1,
                (GLOBALS->num_glitch_regions_ghw_c_1 != 1) ? "s" : "");

    return GLOBALS->max_time;
}

/*******************************************************************************/
