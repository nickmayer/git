/*
 * Builtin "git subtree" and related commands
 *
 * (C) Copyright 2011 Nick Mayer
 *
 * Based on git-subtree.sh by Avery Pennarun and Jesse Greenwald's subtree
 * patch for jgit.
 */

#include "builtin.h"
#include "subtree.h"
#include "cache-tree.h"
#include "dir.h"
#include "parse-options.h"
#include "refs.h"
#include "revision.h"
#include "run-command.h"
#include "tree-walk.h"
#include "unpack-trees.h"

/*---------------------------------------------------------------------------*/
/*              #####  ####### #     # #     # ####### #     #               */
/*             #     # #     # ##   ## ##   ## #     # ##    #               */
/*             #       #     # # # # # # # # # #     # # #   #               */
/*             #       #     # #  #  # #  #  # #     # #  #  #               */
/*             #       #     # #     # #     # #     # #   # #               */
/*             #     # #     # #     # #     # #     # #    ##               */
/*              #####  ####### #     # #     # ####### #     #               */
/*---------------------------------------------------------------------------*/

static int debug_printf_enabled = 1;
#define debug(...) if (debug_printf_enabled) fprintf(stderr, __VA_ARGS__)

/*-----------------------------------------------------------------------------
Options parsing for string lists
-----------------------------------------------------------------------------*/
static int opt_string_list(const struct option *opt, const char *arg, int unset)
{
    struct string_list *exclude_list = opt->value;
    string_list_append(exclude_list, arg);
    return 0;
}

/*-----------------------------------------------------------------------------

-----------------------------------------------------------------------------*/
void fetch_branch(const char* remote, const char* branch)
{
    const char *fetch_argv[10];
    int fetch_result;
    int i = 0;
    fetch_argv[i++] = "fetch";
    fetch_argv[i++] = remote;
    if (branch)
        fetch_argv[i++] = branch;
    fetch_argv[i++] = "--quiet";
    fetch_argv[i] = NULL;
    fetch_result = cmd_fetch(i, fetch_argv, "");
    if (fetch_result)
        die("Unable to fetch (%d)", fetch_result);
}

/*-----------------------------------------------------------------------------
Create the squash commit.
TODO: Pass in enough information for us to walk the history and build a
detailed commit message that has the squash history
-----------------------------------------------------------------------------*/
struct commit *create_squash_commit(struct tree *original,
                                    struct commit_list *parents,
                                    const char *squash_info)
{
    unsigned char result_commit[20];
    struct strbuf commit_msg = STRBUF_INIT;
    strbuf_addstr(&commit_msg, "Subtree squash ");
    strbuf_addstr(&commit_msg, squash_info);

    commit_tree
        (
        commit_msg.buf,
        original->object.sha1,
        parents,
        result_commit,
        NULL,
        NULL
        );
    strbuf_release(&commit_msg);

    return lookup_commit(result_commit);
}

/*---------------------------------------------------------------------------*/
/*                             #    ######  ######                           */
/*                            # #   #     # #     #                          */
/*                           #   #  #     # #     #                          */
/*                          #     # #     # #     #                          */
/*                          ####### #     # #     #                          */
/*                          #     # #     # #     #                          */
/*                          #     # ######  ######                           */
/*---------------------------------------------------------------------------*/

struct add_opts {
    int no_dot_subtree;
    int squash;
    const char* remote;
    const char* name;
    const char* prefix;
};

static const char * const builtin_subtree_add_usage[] = {
    "git subtree add -P <prefix> [-n | --name] [(-r | --remote)=<remote>] [--squash] <branch>",
    NULL
};

/*-----------------------------------------------------------------------------

-----------------------------------------------------------------------------*/
static int cmd_subtree_add(int argc, const char **argv, const char *prefix)
{
    struct add_opts opts;
    int newfd;
    unsigned int i;
    struct lock_file lock_file;
    struct tree *tree;
    struct tree_desc tree_desc;
    struct unpack_trees_options unpack_opts;
    unsigned char merge_sha1[20];
    unsigned char result_tree[20];
    unsigned char result_commit[20];
    struct commit_list* parents = NULL;
    struct strbuf commit_msg = STRBUF_INIT;
    struct checkout state;
    static char topath[PATH_MAX + 1];
    const char *pathspec[2];
    const char *branch_merge_name;
    const char *branch_name;
    struct option options[] = {
        OPT_STRING('r', "remote", &opts.remote, "repo", "Location of external repository to fetch branch from"),
        OPT_BOOLEAN(0, "squash", &opts.squash, "Bring history in as one commit"),
        OPT_STRING('n', "name", &opts.name, "subtree name", "Name of the subtree"),
        OPT_STRING('P', "prefix", &opts.prefix, "prefix", "Location to add subtree"),
        OPT_END(),
        };

    /* Parse arguments */
    memset(&opts, 0, sizeof(opts));
    argc = parse_options(argc, argv, prefix, options, builtin_subtree_add_usage, PARSE_OPT_KEEP_DASHDASH);

    /* TODO: Verify this prefix doesn't already exist in the tree (or locally)? */
    if (opts.prefix == NULL) {
        die("git subtree add: must specify prefix");
    }

    if (argc != 1 && !opts.remote) {
        die("git subtree add: branch must be specified");
    }

    newfd = hold_locked_index(&lock_file, 1);

    if (read_cache_unmerged())
        die("You need to resolve your current index first");

    if (opts.remote) {
        fetch_branch(opts.remote, argc > 0 ? argv[0] : NULL);
        branch_merge_name = "FETCH_HEAD";
        branch_name = argc > 0 ? argv[0] : "master";
    }
    else {
        branch_merge_name = argv[0];
        branch_name = branch_merge_name;
    }

    /* TODO: Add option to fetch from a remote first, then use FETCH_HEAD to get sha1. */
    if (get_sha1(branch_merge_name, merge_sha1))
        die("git subtree add: Valid branch must be specified");

    debug("Add commit %s\n", sha1_to_hex(merge_sha1));

    tree = parse_tree_indirect(merge_sha1);
    init_tree_desc(&tree_desc, tree->buffer, tree->size);

    git_config(git_default_config, NULL);
    setup_work_tree();

    memset(&unpack_opts, 0, sizeof(unpack_opts));
    unpack_opts.head_idx = 1;
    unpack_opts.src_index = &the_index;
    unpack_opts.dst_index = &the_index;
    unpack_opts.prefix = opts.prefix;
    unpack_opts.merge = 1;
    unpack_opts.update = 1;
    unpack_opts.fn = bind_merge;

    cache_tree_free(&active_cache_tree);
    if (unpack_trees(1, &tree_desc, &unpack_opts)) {
        rollback_lock_file(&lock_file);
        die("Unable to read tree");
    }

    /* checkout */
    strcpy(topath, prefix);
    memset(&state, 0, sizeof(state));
    state.base_dir = "";
    state.force = 1;
    state.refresh_cache = 1;
    pathspec[0] = opts.prefix;
    pathspec[1] = NULL;
    for (i = 0; i < the_index.cache_nr; i++)  {
        struct cache_entry *ce = the_index.cache[i];
        if (match_pathspec(pathspec, ce->name, ce_namelen(ce), 0, NULL)) {
            if (!ce_stage(ce)) {
                checkout_entry(ce, &state, NULL);
                continue;
            }
        }
    }

    /* If a name was given, we'll record the info in the .gitsubtree file */
    if (opts.name) {
        struct strbuf config_key = STRBUF_INIT;
        config_exclusive_filename = ".gitsubtree";
        strbuf_addstr(&config_key, "subtree.");
        strbuf_addstr(&config_key, opts.name);
        if (opts.remote) {
            strbuf_addstr(&config_key, ".url");
            git_config_set(config_key.buf, opts.remote);
            strbuf_setlen(&config_key, config_key.len - 4);
        }
        strbuf_addstr(&config_key, ".path");
        git_config_set(config_key.buf, opts.prefix);
        strbuf_release(&config_key);

        config_exclusive_filename = NULL;

        /* Stage .gitsubtree */
        add_file_to_cache(".gitsubtree", 0);
    }

    if (write_cache(newfd, active_cache, active_nr) || commit_locked_index(&lock_file))
        die("unable to write new index file");

    /* At this point things are staged & in the index, but not committed */
    parents = NULL;
    if (opts.squash) {
        struct commit *commit;
        commit = lookup_commit(merge_sha1);
        parse_commit(commit);
        commit = create_squash_commit(commit->tree, NULL, branch_name);
        commit_list_insert(commit, &parents);
    }
    else {
        commit_list_insert(lookup_commit(merge_sha1), &parents);
    }
    commit_list_insert(lookup_commit_reference_by_name("HEAD"), &parents);

    if (write_cache_as_tree(result_tree, 0, NULL))
        die("git write-tree failed to write a tree");

    strbuf_addstr(&commit_msg, "Subtree add ");
    strbuf_addstr(&commit_msg, branch_name);
    if (opts.remote) {
        strbuf_addstr(&commit_msg, " on ");
        strbuf_addstr(&commit_msg, opts.remote);
    }
    strbuf_addstr(&commit_msg, " into ");
    strbuf_addstr(&commit_msg, opts.prefix);

    commit_tree(commit_msg.buf, result_tree, parents, result_commit, NULL, NULL);
    strbuf_release(&commit_msg);

    printf("%s\n", sha1_to_hex(result_commit));

    /* Now we just need to move the current tree up to the newly created commit */
    update_ref("subtree add", "HEAD", result_commit, NULL, 0, DIE_ON_ERR);
    return 0;
}

/*---------------------------------------------------------------------------*/
/*                        #       ###  #####  #######                        */
/*                        #        #  #     #    #                           */
/*                        #        #  #          #                           */
/*                        #        #   #####     #                           */
/*                        #        #        #    #                           */
/*                        #        #  #     #    #                           */
/*                        ####### ###  #####     #                           */
/*---------------------------------------------------------------------------*/

struct list_opts {
    int exact;
    struct string_list prefix_list;
};

static const char * const builtin_subtree_list_usage[] = {
    "git subtree list [options] <paths>",
    "git subtree list [options] <branch> <paths>",
    "git subtree list [options] [<branch>] -- <paths>",
    NULL,
};

/*-----------------------------------------------------------------------------

-----------------------------------------------------------------------------*/
static int cmd_subtree_list(int argc, const char **argv, const char *prefix)
{
    struct list_opts opts;
    struct option options[] = {
        OPT_BOOLEAN(0, "exact", &opts.exact, "Only list exact subtree matches"),
        OPT_CALLBACK('P', "prefix", &opts.prefix_list, "prefix", "prefix <path>", opt_string_list),
        OPT_END(),
        };
    struct commit_list *subtree_commits = NULL;
    struct rev_info rev;
    struct setup_revision_opt opt;
    struct commit *commit;
    static struct string_list prefix_list;

    
    /* Parse arguments */
    memset(&opts, 0, sizeof(opts));
    argc = parse_options(argc, argv, prefix, options, builtin_subtree_list_usage, PARSE_OPT_KEEP_DASHDASH);
    
    /*
     * Setup the walk. Make sure the user didn't pass any flags that will
     * mess things up
     */
    init_revisions(&rev, prefix);
    rev.topo_order = 1;
    rev.reverse = 0;
    rev.bisect = 0;
    rev.ignore_merges = 0;
	rev.max_parents = -1;
	rev.min_parents = 2; /* There won't be a subtree merge without a merge */

    memset(&opt, 0, sizeof(opt));
    opt.def = "HEAD";

    /* HACK: argv[0] is ignored by setup_revisions */
    argc = setup_revisions(argc+1, argv-1, &rev, &opt) - 1;
    if (argc)
        die("Unknown option: %s", argv[0]);

    prepare_revision_walk(&rev);
    while ((commit = get_revision(&rev))) {
        subtree_commits = get_subtrees(commit, &opts.prefix_list, opts.exact);

        /*
         * TODO: If this commit has been identified as being in a subtree, 
         * don't try and look for the .subtree or splitting it? This would
         * require we propagate referenced and not only look at merges.
         */

        while ((commit = pop_commit(&subtree_commits))) {
            printf("%s\n", sha1_to_hex(commit->object.sha1));
        }
    }

    return 0;
}

/*---------------------------------------------------------------------------*/
/*                  #     # ####### ######   #####  #######                  */
/*                  ##   ## #       #     # #     # #                        */
/*                  # # # # #       #     # #       #                        */
/*                  #  #  # #####   ######  #  #### #####                    */
/*                  #     # #       #   #   #     # #                        */
/*                  #     # #       #    #  #     # #                        */
/*                  #     # ####### #     #  #####  #######                  */
/*---------------------------------------------------------------------------*/

static const char * const builtin_subtree_merge_usage[] = {
    "git subtree merge -P <prefix> [-r | --remote=<remote>] [--squash] <branch> [merge options]",
    NULL
};

struct merge_opts {
    int squash;
    const char *remote;
    struct string_list prefix_list;
};

static int cmd_subtree_merge(int argc, const char **argv, const char *prefix)
{
    int retval;
    int i, j;
    struct merge_opts opts;
    struct strbuf subtree_strategy = STRBUF_INIT;
    struct strbuf subtree_message = STRBUF_INIT;
    const char **subtree_argv;
    const char *branch_name;
    struct option options[] = {
        OPT_BOOLEAN(0, "squash", &opts.squash, "Bring history in as one commit"),
        OPT_CALLBACK('P', "prefix", &opts.prefix_list, "prefix", "prefix <path>", opt_string_list), /* TODO: Support multiples somehow (named subtrees only?) */
        OPT_STRING('r', "remote", &opts.remote, "repo", "Location of external repository to merge branch from"),
        OPT_END()
    };

    memset(&opts, 0, sizeof(opts));
    argc = parse_options(argc, argv, prefix, options, builtin_subtree_merge_usage, PARSE_OPT_KEEP_UNKNOWN);

    if (opts.prefix_list.nr == 0) {
        /* TODO: Determine prefix from current directory if not given? */
        error("Must specify a prefix");
        usage_with_options(builtin_subtree_merge_usage, options);
    }
    if (opts.prefix_list.nr > 1) {
        die("You can only subtree merge one subtree at a time");
        usage_with_options(builtin_subtree_merge_usage, options);
    }

    branch_name = argc > 0 ? argv[0] : NULL;

    /* TODO: This is the same as subtree pull...unify? Have pull call this since we support squash here */
    if (opts.remote) {
        fetch_branch(opts.remote, branch_name);
        branch_name = "FETCH_HEAD";
    }

    /*
     * If we're squashing, we need to create a new commit that contains the
     * tree of the given commit and has a parent of the last subtree merge
     * for the given prefix
     */
    if (opts.squash) {
        struct commit *commit;
        struct commit *parent;
        struct commit_list *parents;
        struct rev_info rev;
        struct setup_revision_opt opt;
        struct commit_list *subtree_commits = NULL;

        /*
         * Walk to find the last subtree merge for the prefix
         */
        init_revisions(&rev, prefix);
        rev.topo_order = 1;
        memset(&opt, 0, sizeof(opt));
        opt.def = "HEAD";
        setup_revisions(0, NULL, &rev, &opt);

        prepare_revision_walk(&rev);
        parent = NULL;
        while ((commit = get_revision(&rev)) && subtree_commits == NULL) {
            subtree_commits = get_subtrees(commit, &opts.prefix_list, 0);
        }

        parents = NULL;
        if (subtree_commits)
            commit_list_insert(subtree_commits->item, &parents);

        free_commit_list(subtree_commits);

        commit = lookup_commit_reference_by_name(branch_name);
        if (!commit)
            die("Unable to lookup branch %s", branch_name);
        parse_commit(commit);

        if (parents && commit->tree == parents->item->tree) {
            die("No new changes");
        }

        commit = create_squash_commit(commit->tree, parents, opts.prefix_list.items[0].string);
        branch_name = sha1_to_hex(commit->object.sha1);
    }

    strbuf_addstr(&subtree_strategy, "-Xsubtree=");
    strbuf_addstr(&subtree_strategy, opts.prefix_list.items[0].string);

    strbuf_addstr(&subtree_message, "Subtree merge ");
    strbuf_addstr(&subtree_message, argc ? argv[0] : "master");
    strbuf_addstr(&subtree_message, " into ");
    strbuf_addstr(&subtree_message, opts.prefix_list.items[0].string);

    subtree_argv = xmalloc(sizeof(char**) * (argc + 10));
    i = 0;
    subtree_argv[i++] = "merge";
    subtree_argv[i++] = subtree_strategy.buf;
    subtree_argv[i++] = "--message";
    subtree_argv[i++] = subtree_message.buf;
    for (j = 0; j < argc - 1; j++) {
        subtree_argv[i+j] = argv[j+1];
    }
    if (branch_name)
        subtree_argv[i++] = branch_name;
    subtree_argv[i+j] = NULL;

    /* Call into merge to actually do the work */
    retval = cmd_merge(i+j, subtree_argv, prefix);

    free(subtree_argv);
    strbuf_release(&subtree_strategy);
    strbuf_release(&subtree_message);
    return retval;
}

/*---------------------------------------------------------------------------*/
/*                      ######  #     # #       #                            */
/*                      #     # #     # #       #                            */
/*                      #     # #     # #       #                            */
/*                      ######  #     # #       #                            */
/*                      #       #     # #       #                            */
/*                      #       #     # #       #                            */
/*                      #        #####  ####### #######                      */
/*---------------------------------------------------------------------------*/

static const char * const builtin_subtree_pull_usage[] = {
    "git subtree pull [options] [<repository> [<refspec>]]",
    NULL,
};

struct pull_opts {
    const char* prefix;
};

/* TODO: Replace this with a call to merge with the --remote option */
static int cmd_subtree_pull(int argc, const char **argv, const char *prefix)
{
    int retval;
    int i, j;
    struct pull_opts opts;
    struct strbuf subtree_strategy = STRBUF_INIT;
    const char **subtree_argv;
    struct option options[] = {
        OPT_STRING('P', "prefix", &opts.prefix, "prefix", "Location to add subtree"),
        OPT_END()
    };

    memset(&opts, 0, sizeof(opts));
    argc = parse_options(argc, argv, prefix, options, builtin_subtree_merge_usage, 0);

    if (!opts.prefix)
    {
        /* TODO: Determine prefix from current directory? */
        error("Must specify a prefix");
        usage_with_options(builtin_subtree_merge_usage, options);
    }

    strbuf_addstr(&subtree_strategy, "-Xsubtree=");
    strbuf_addstr(&subtree_strategy, opts.prefix);

    subtree_argv = xmalloc(sizeof(char**) * (argc + 5));
    i = 0;
    subtree_argv[i++] = "pull";
    subtree_argv[i++] = subtree_strategy.buf;
    for (j = 0; j < argc; j++)
    {
        subtree_argv[i+j] = argv[j];
    }
    subtree_argv[i+j] = NULL;

    /* Call into pull to actually do the work */
    retval = run_command_v_opt(subtree_argv, RUN_GIT_CMD);

    strbuf_release(&subtree_strategy);
    return retval;
}

/*---------------------------------------------------------------------------*/
/*                      ######  #     #  #####  #     #                      */
/*                      #     # #     # #     # #     #                      */
/*                      #     # #     # #       #     #                      */
/*                      ######  #     #  #####  #######                      */
/*                      #       #     #       # #     #                      */
/*                      #       #     # #     # #     #                      */
/*                      #        #####   #####  #     #                      */
/*---------------------------------------------------------------------------*/

static const char * const builtin_subtree_push_usage[] = {
    "git subtree push",
    NULL,
};

static int cmd_subtree_push(int argc, const char **argv, const char *prefix)
{
    /* TODO */
    return 0;
}

/*---------------------------------------------------------------------------*/
/*                     #####  ######  #       ### #######                    */
/*                    #     # #     # #        #     #                       */
/*                    #       #     # #        #     #                       */
/*                     #####  ######  #        #     #                       */
/*                          # #       #        #     #                       */
/*                    #     # #       #        #     #                       */
/*                     #####  #       ####### ###    #                       */
/*---------------------------------------------------------------------------*/

struct split_opts {
    int rewrite_head;
    int rewrite_parents;
    int change_committer;
    int rejoin;
    int squash;
    const char *annotation;
    const char *footer;
    struct string_list onto_list;
    struct string_list prefix_list;
    const char *output;             /* TODO: Output format to show all commits, just head(s), commits by prefix, etc */
};

static struct commit_list *onto_list;

struct subtree_details
{
    struct commit *commit;
    int nr;
    struct 
    {
	    const char *prefix;
	    size_t len;
        struct tree *tree;
    } items[];
};
static struct subtree_details *g_details;

struct commit_util {
    struct commit_list *remapping;
    struct tree *tree;
    /* Is this a subtree or supertree commit that is fully resolved */
    unsigned int referenced : 1;
    /* Override referenced */
    unsigned int force : 1;
    /* Is this commit on the subtree (meaning remapping points to original commit) */
    unsigned int is_subtree : 1;
    /* Did we create this commit */
    unsigned int created;
};

/*-----------------------------------------------------------------------------

-----------------------------------------------------------------------------*/
__inline struct commit_util *get_commit_util(struct commit *commit,
                                             int index,
                                             int create) /* =2 if assert on non-existence */
{
    if(!commit->util) {
        int sz;
        assert(create != 2);
        if (!create)
            return NULL;

        /* Allocate one for each prefix, and an extra for self */
        sz = sizeof(struct commit_util) * (g_details->nr + 1);
        /* TODO: Find a way to eventually free this memory? */
        commit->util = malloc(sz);
        memset(commit->util, 0, sz);
    }
    return &((struct commit_util*)commit->util)[index];
}

/*-----------------------------------------------------------------------------

-----------------------------------------------------------------------------*/
__inline void debug_commit(struct commit *commit, int index) {
    struct commit_util *util;

    parse_commit(commit);
    util = get_commit_util(commit, index, 0);
    
    debug("------------------------------------------------\n");
    debug("Commit: %s\n", sha1_to_hex(commit->object.sha1));
    debug("Tree: %s\n", sha1_to_hex(commit->tree->object.sha1));
    debug("Util(%d): \"%s\"\n", index, g_details->items[index].prefix);
    if (util) {
        struct commit_list *temp;
        debug("\tCreated: %d\n", util->created);
        debug("\tForce: %d\n", util->force);
        debug("\tReferenced: %d\n", util->referenced);
        debug("\tIs Subtree: %d\n", util->is_subtree);
        debug("\tTree: %s\n", util->tree ? sha1_to_hex(util->tree->object.sha1) : "");
        debug("\tRemapping(s):\n");
        temp = util->remapping;
        while (temp) {
            debug("\t\t%s\n", sha1_to_hex(temp->item->object.sha1));
            temp = temp->next;
        }
    }
    else {
        debug("\t<null>\n");
    }
    debug("------------------------------------------------\n");
}

/*-----------------------------------------------------------------------------

-----------------------------------------------------------------------------*/
static int read_subtree_config(const char *var, const char *value, void *context)
{
    struct string_list *config = context;
    const char* substr = NULL;

    if ((substr = strstr(var, "subtree."))) {
        const char* dot;
        substr += 8; /* Skip past subtree. */
        if ((dot = strstr(substr, "."))) {
            dot += 1; /* Skip past the . */
            if( strstr( dot, "path" ) ) {
                struct strbuf tmp = STRBUF_INIT;
                strbuf_addstr(&tmp, value);
                string_list_append(config, tmp.buf);
                config->items[config->nr - 1].util = (void*) ((intptr_t) strlen(tmp.buf));
            }
        }

    }
    return 0;
}

/*-----------------------------------------------------------------------------

-----------------------------------------------------------------------------*/
static void setup_prefix_list(struct string_list *prefix_list)
{
    int i;
    struct string_list subtree_list = STRING_LIST_INIT_NODUP;

    if (prefix_list->nr == 0) {
        /*
         * Read the prefixes from .gitsubtree
         */
        git_config_from_file(read_subtree_config, ".gitsubtree", &subtree_list);
        prefix_list = &subtree_list;
    }

    g_details = xrealloc(g_details, sizeof(*g_details) + sizeof(*(g_details->items)) * prefix_list->nr);
    for (i = 0; i < prefix_list->nr; i++) {
        const char* str = prefix_list->items[i].string;
        g_details->items[i].prefix = str;
        g_details->items[i].len = strlen(str);
        g_details->items[i].tree = NULL;
    }
    g_details->nr = prefix_list->nr;

    string_list_clear(&subtree_list, 0);
}

/*-----------------------------------------------------------------------------
Find the tree sha1 values of the given prefixes and store them in pathinfo
-----------------------------------------------------------------------------*/
static int read_tree_find_subtrees(const unsigned char *sha1, const char *base,
                                   int baselen, const char *pathname,
                                   unsigned mode, int stage, void *context)
{
    int result = 0;
    int i;
    int pathlen;
    struct commit *commit = context;

    if (!S_ISDIR(mode)) {
        /* This isn't a folder, so we can't split off of it */
        return result;
    }

    pathlen = strlen(pathname);
    for (i = 0; i < g_details->nr; i++) {
        char *prefix;
        int prefix_len;
        struct commit_util *util;

        /*
         * Don't bother with ignored subtrees other than to propagate the
         * ignore. 
         */
        util = get_commit_util(commit, i, 0);
        if (util && util->referenced)
            continue;
       
        prefix = g_details->items[i].prefix;
        prefix_len = g_details->items[i].len;

        if (baselen > prefix_len)
            continue;
        if (strncmp(prefix, base, baselen) != 0)
            continue;

        if (strncmp(pathname, &prefix[baselen], pathlen) == 0) {
            if (baselen + pathlen == prefix_len)
                g_details->items[i].tree = lookup_tree(sha1);
            else
                result = READ_TREE_RECURSIVE;
        }
    }

    return result;
}

/*-----------------------------------------------------------------------------
Find the subtree tree SHA1 for the given commit based on the subtrees in 
g_details
-----------------------------------------------------------------------------*/
void find_subtrees(struct commit *commit)
{
    int i;
    int work_to_do = 0;

    /*
     * First, propagate the referenced flags to children
     */
    for (i = 0; i < g_details->nr; i++) {
        struct commit_util *util;
        util = get_commit_util(commit, i, 0);
        if (util && util->referenced) {
            struct commit_list *parent = commit->parents;
            while (parent) {
                get_commit_util(parent->item, i, 1)->referenced |= util->referenced;
                parent = parent->next;
            }
            if (util->force)
                util->referenced = 0;
            else
                continue;
        }
        work_to_do = 1;
    }

    /*
     * Read the tree to get the subtree's SHA1 (if it exists)
     */
    for (i = 0; i < g_details->nr; i++)
        g_details->items[i].tree = NULL;
    if (work_to_do)
        read_tree_recursive(commit->tree, "", 0, 0, NULL, read_tree_find_subtrees, commit);
}

/*-----------------------------------------------------------------------------
compare_trees
Compare 2 trees, count added, removed, changed, and unchanged files
-----------------------------------------------------------------------------*/
enum tree_compare_result {
    TREE_SAME,
    TREE_MODIFIED,
    TREE_DIFFERENT
};

struct compare_details {
    unsigned int add;
    unsigned int remove;
    unsigned int same;
    unsigned int change;
};

static enum tree_compare_result compare_trees(struct tree *tree1,
                                              struct tree *tree2,
                                              int recurse,
                                              struct compare_details *details)
{
    struct tree_desc t1, t2;
    int pathlen1;
    int pathlen2;
    int cmp;
    struct compare_details detail;
    memset(&detail, 0, sizeof(detail));

    /* Count up the number of matching tree objects between left & right */
    parse_tree(tree1);
    parse_tree(tree2);
    init_tree_desc(&t1, tree1->buffer, tree1->size);
    init_tree_desc(&t2, tree2->buffer, tree2->size);

    while (t1.size || t2.size) {
        pathlen1 = tree_entry_len(t1.entry.path, t1.entry.sha1);
        pathlen2 = tree_entry_len(t2.entry.path, t2.entry.sha1);
        if (t1.size == 0)
            cmp = -1;
        else if (t2.size == 0)
            cmp = 1;
        else
            cmp = base_name_compare(t1.entry.path, pathlen1, t1.entry.mode,
                                    t2.entry.path, pathlen2, t2.entry.mode);
        if (cmp < 0) {
            /* Removed from tree1 */
            detail.remove++;
        }
        else if (cmp > 0) {
            /* Added to tree 1 */
            detail.add++;
        }
        else if (t1.entry.mode == t2.entry.mode &&
                 hashcmp(t1.entry.sha1, t2.entry.sha1) == 0) {
            /* No changes */
            detail.same++;
        }
        else {
            /* Changed */
            if (recurse && S_ISDIR(t1.entry.mode) && S_ISDIR(t2.entry.mode)) {
                compare_trees(lookup_tree(t1.entry.sha1),
                              lookup_tree(t2.entry.sha1),
                              recurse, &detail);
            }
            else {
                detail.change++;
            }
        }

        update_tree_entry(&t1);
        update_tree_entry(&t2);
    }

    if (details) {
        details->add += detail.add;
        details->change += detail.change;
        details->remove += detail.remove;
        details->same += detail.same;
    }

    if (detail.same > 0) {
        if (detail.add + detail.change + detail.remove > 0)
            return TREE_MODIFIED;
        else
            return TREE_SAME;
    }
    else
        return TREE_DIFFERENT;
}

/*-----------------------------------------------------------------------------

-----------------------------------------------------------------------------*/
static struct commit *find_subtree_parent(struct commit *commit,
                                          int prefix_index,
                                          int exact_match,
                                          int update_util)
{
    struct commit_list *parent;
    struct commit *best_commit = NULL;
    struct tree *tree;

    tree = g_details->items[prefix_index].tree;

    /* Check our parents to see if this tree matches the tree node there */
    parent = commit->parents;
    while (parent)
    {
        struct commit *c = parent->item;
        if (c->tree == tree)
        {
            best_commit = c;
            break;
        }
        parent = parent->next;
    }

    /* 
     * If we allow non-exact matches, lets try a bit harder to see how close
     * we are by ranking each parent and picking the highest non-zero ranking
     * match.
     */
    if (!best_commit && !exact_match)
    {
        unsigned int best_val = 0;
        unsigned int alt_val = UINT_MAX;
        parent = commit->parents;
        while (parent)
        {
            struct compare_details matches;
            struct commit *c = parent->item;
            memset(&matches, 0, sizeof(matches));
            parse_commit(c);
            compare_trees(tree, c->tree, 1, &matches);
            if (matches.same > best_val) {
                best_val = matches.same;
                best_commit = c;
            }
            /* 
             * If we don't have anything with any matches, pick the thing that
             * has many similarly named files, if there aren't more adds/removes
             * than there are changes
             */
            if (best_val == 0
                && alt_val > (matches.add + matches.remove) 
                && matches.change > (matches.add + matches.remove)) {

                alt_val = (matches.add + matches.remove);
                best_commit = c;
            }
            parent = parent->next;
        }
    }

    if (update_util && best_commit) {
        struct commit_util *util;

        /*
         * We've found an existing subtree commit. Set the
         * remap info so we know not to try to create a new
         * commit in the next stage
         */
        util = get_commit_util(commit, prefix_index, 1);
        commit_list_insert(best_commit, &util->remapping);
        util->tree = tree;
        util->referenced = 0;

        /*
         * We'll propagate this as we see subtree commits to
         * save on setting this on things we may never even
         * go far enough to see.
         * TODO: How will this work with --all passed in as a
         * refspec and a feature branch that removed the subtree?
         * Setting a flag on the parent saying it needs to
         * process that can override the don't process flag
         * would work I guess..
         */
        parent = commit->parents;
        while (parent) {
            util = get_commit_util(parent->item, prefix_index, 1);
            util->referenced = 1;
            util->is_subtree = (parent->item == best_commit);
            parent = parent->next;
        }
    }

    return best_commit;
}

/*-----------------------------------------------------------------------------
Store the SHA1 of the .subtree file in the strbuf pointed to by context
-----------------------------------------------------------------------------*/
static int get_subtree_sha1_read_tree(const unsigned char *sha1, const char *base, 
                                      int baselen, const char *pathname, 
                                      unsigned mode, int stage, void *context)
{
    int retval = 0;

    if (S_ISGITLINK(mode)) {
        return 0;
    } else if (S_ISDIR(mode)) {
        /* TODO: Support nested subtrees, or is that just a horrible idea? */
        //return READ_TREE_RECURSIVE;
        return 0;
    }

    if( strcmp( pathname, ".gitsubtree" ) == 0 ) {
        strbuf_addstr(context, (char *)sha1);
        /* Found it, stop looking */
        return -1;
    }

    return retval;
}

/*-----------------------------------------------------------------------------

-----------------------------------------------------------------------------*/
static struct commit *rewrite_commit(struct commit *commit,
                                     struct tree *tree,
                                     struct commit_list *remapped_parents,
                                     int is_subtree,
                                     struct split_opts *opts)
{
    const char* body;
    const char* author;
    const char* committer;
    int len;
    struct strbuf body_str = STRBUF_INIT;
    struct strbuf author_str = STRBUF_INIT;
    struct strbuf committer_str = STRBUF_INIT;
    unsigned char output_commit_sha1[20];

    /* The commit buffer contains tree-id, parents, etc. */
    len = find_commit_subject(commit->buffer, &body);
    len = find_commit_author(commit->buffer, &author);
    strbuf_add(&author_str, author, len);
    author = author_str.buf;

    /*
     * TODO: Take a param so they can optionally specify committer info
     * instead of reading from environment?
     */
    if (!opts->change_committer)
    {
        len = find_commit_committer(commit->buffer, &committer);
        strbuf_add(&committer_str, committer, len);
        committer = committer_str.buf;
    }
    else
    {
        committer = NULL;
    }

    if (is_subtree && opts->annotation)
    {
        strbuf_addstr(&body_str, opts->annotation);
    }
    strbuf_addstr(&body_str, body);
    if (is_subtree && opts->footer)
    {
        strbuf_addstr(&body_str, opts->footer);
    }
    body = body_str.buf;

    /* Create a new commit object */
    hashclr(output_commit_sha1);
    commit_tree(body, tree->object.sha1, remapped_parents, output_commit_sha1, author, committer);
    strbuf_release(&author_str);
    strbuf_release(&committer_str);
    strbuf_release(&body_str);
    /* commit_tree frees remapped_parents for us */

    return lookup_commit(output_commit_sha1);
}

/*-----------------------------------------------------------------------------
Walk the tree and make a list of all commits that may potentially need to be
split into subtree commits.
-----------------------------------------------------------------------------*/
static struct commit_list *get_interesting_split_commits(int argc, const char **argv, const char *prefix)
{
    struct commit_list *interesting_commits = NULL;
    struct rev_info rev;
    struct setup_revision_opt opt;
    struct commit *commit;
    int has_subtree_data;
    int i;

    /*
     * Setup the walk. Make sure the user didn't pass any flags that will
     * mess things up
     */
    init_revisions(&rev, prefix);
    rev.topo_order = 1;
    rev.reverse = 0;
    rev.bisect = 0;
    rev.ignore_merges = 0;
	rev.max_parents = -1;
	rev.min_parents = 0;

    memset(&opt, 0, sizeof(opt));
    opt.def = "HEAD";

    /* HACK: argv[0] is ignored by setup_revisions */
    argc = setup_revisions(argc+1, argv-1, &rev, &opt) - 1;
    if (argc)
        die("Unknown option: %s", argv[0]);

    prepare_revision_walk(&rev);
    while ((commit = get_revision(&rev))) {
        debug("%s processing...\n", sha1_to_hex(commit->object.sha1));

        find_subtrees(commit);
        has_subtree_data = 0;
        for (i = 0; i < g_details->nr; i++) {
            if (g_details->items[i].tree) {
                struct commit_util *util;
                struct commit_list *onto;
                struct commit_list *parents;
                struct commit *parent = NULL;

                /*
                 * If the tree id matches one of the onto trees, we don't need
                 * to search any further
                 */
                onto = onto_list;
                while (onto) {
                    if (g_details->items[i].tree == onto->item->tree) {

                        debug("\tFound onto %s for %s\n",
                            sha1_to_hex(onto->item->object.sha1),
                            g_details->items[i].prefix);

                        util = get_commit_util(commit, i, 1);
                        commit_list_insert(onto->item, &util->remapping);
                        util->tree = onto->item->tree;
                        util->referenced = 0;

                        parents = commit->parents;
                        while (parents) {
                            util = get_commit_util(parents->item, i, 1);
                            util->referenced = 1;
                            util->is_subtree = (parents->item == onto->item);
                            parents = parents->next;
                        }
                        /* TODO: Remove from the onto list */
                        break;
                    }
                    onto = onto->next;
                }
                if (onto)
                    continue;

                /*
                 * Check to see if one of this commit's parents is already the subtree
                 * merge we're going to be generating
                 */
                parent = find_subtree_parent(commit, i, 0, 1);
                if (parent) {
                    debug("\tFound existing subtree parent %s for %s\n",
                        sha1_to_hex(parent->object.sha1),
                        g_details->items[i].prefix);
                    /* 
                     * If the trees aren't the same, it is still interesting.
                     * It means that somebody amended the subtree merge commit.
                     */
                    if (parent->tree == g_details->items[i].tree)
                        continue;
                }
                
                util = get_commit_util(commit, i, 1);
                util->referenced = 0;
                util->tree = g_details->items[i].tree;

                debug("\tFound tree %s for %s\n",
                    sha1_to_hex(g_details->items[i].tree->object.sha1),
                    g_details->items[i].prefix);

                /*
                 * This tree has some potential subtree data. We need to mark
                 * it's parents so we know they'll need to be processed. This
                 * is necessary so a branch doesn't cause us to ignore data
                 * for a parallel branch.
                 */
                parents = commit->parents;
                while (parents) {
                    util = get_commit_util(parents->item, i, 1);
                    util->force = 1;
                    parents = parents->next;
                }

                has_subtree_data = 1;
            }
        }

        /* Add this commit to the list for processing (in reverse order) */
        if (has_subtree_data)
            commit_list_insert(commit, &interesting_commits);
    }

    return interesting_commits;
}


/*-----------------------------------------------------------------------------
Returns true if parents were cleaned up and we no longer need to create a commit
-----------------------------------------------------------------------------*/
int cleanup_remapped_parents(struct commit *commit, int index, struct commit_list **remapped_parents_ptr) 
{
    int is_skip_rewrite = 0;
    struct commit_list **insert;
    struct commit_list *remapped_parents = *remapped_parents_ptr;
    struct commit_util *commit_util;

    commit_util = get_commit_util(commit, index, 2);

    if (remapped_parents && remapped_parents->next) {
        struct commit_list *search_list = NULL;
        struct commit_list *next_list;
        struct commit_list **next;
        struct commit *search;
        int found_unnecessary = 0;
        unsigned int min_create = UINT_MAX;

        debug("\t\t### Expensive check to validate parents are necessary\n");

        /*
         * We know the element that was created first cannot have
         * elements created after as parents, so we'll only search
         * their ancestors.
         *
         * We also know that there can only be as many nodes
         * between them as the difference between their creation
         * order. Unfortunately we can't guarantee we'll search in
         * that same order so I haven't been able to take advantage
         * of that fact. Also, is that still true when we're rewriting?
         */

        /*
         * Sort the remapped parents by their created time, newest
         * (high number) to oldest (low number)
         */
        next = &remapped_parents;
        while (*next) {
            struct commit_util *next_list_util = get_commit_util((*next)->item, index, 2);

            /* If it isn't a subtree, it is necessary */
            if (!next_list_util->is_subtree) {
                debug("\t\tSkipping %s (not a subtree)\n", sha1_to_hex((*next)->item->object.sha1));
                next = &(*next)->next;
                continue;
            }

            /*
             * If the commit is already created check to see if it
             * is already part of history
             */
            if (next_list_util->created == 0) {
                if (next_list_util->force && next_list_util->referenced) {
                    debug("\t\tSkipping %s (already in history)\n", sha1_to_hex((*next)->item->object.sha1));
                    *next = (*next)->next;
                    found_unnecessary = 1;
                    continue;
                }
            }

            if (min_create > next_list_util->created)
                min_create = next_list_util->created;
            insert = &search_list;
            while (*insert) {
                struct commit_util *insert_util = get_commit_util((*insert)->item, index, 2);
                if (insert_util->created < next_list_util->created)
                    break;
                insert = &(*insert)->next;
            }
            commit_list_insert((*next)->item, insert);
            next = &(*next)->next;
        }

        /*
         * For each item in the search list, search its history for
         * the other commits in the list.
         */
        while ((search = pop_commit(&search_list))) {
            struct commit_util *search_util = get_commit_util(search, index, 2);
            struct commit_list *working_list = NULL;
            struct commit *working_commit;
            struct commit_list *iter_list = NULL;

            debug("\t\tSearch %s (%u)\n", sha1_to_hex(search->object.sha1), search_util->created);

            /*
             * Add the current search item to the working list.
             * We'll process it first, then move on to its
             * parents until we know we've passed the commit
             * we're interested in.
             */
            iter_list = search_list;
            commit_list_insert(search, &working_list);
            while (search_list && (working_commit = pop_commit(&working_list))) {
                struct commit_list *next_parents;
                debug("\t\t\t%s (%u)\n", sha1_to_hex(working_commit->object.sha1), get_commit_util(working_commit, index, 2)->created);

                /* If the working commit is in the search list, it is an unnecessary parent */
                iter_list = search_list;
                while (iter_list) {
                    if (working_commit == iter_list->item) {
                        struct commit_list **prev;
                        /* Remove this commit from the search list & remapped parents */
                        debug("\t\t\t#### Found unnecessary parent %s\n", sha1_to_hex(working_commit->object.sha1));
                        found_unnecessary = 1;
                        prev = &remapped_parents;
                        while (*prev) {
                            if ((*prev)->item == working_commit) {
                                *prev = (*prev)->next;
                                break;
                            }
                            prev = &(*prev)->next;
                        }
                        prev = &search_list;
                        while (*prev) {
                            if ((*prev)->item == working_commit) {
                                *prev = (*prev)->next;
                                break;
                            }
                            prev = &(*prev)->next;
                        }
                        break;
                    }
                    iter_list = iter_list->next;
                }

                next_parents = working_commit->parents;
                while (next_parents) {
                    insert = &working_list;
                    while (*insert) {
                        if ((*insert)->item == next_parents->item)
                            break;
                        insert = &(*insert)->next;
                    }
                    if (*insert == NULL) {
                        struct commit_util *next_util = get_commit_util(next_parents->item, index, 2);
                        if (next_util->created >= min_create)
                            commit_list_insert(next_parents->item, insert);
                    }
                    next_parents = next_parents->next;
                }
            }
            free_commit_list(working_list);

        }

        /*
         * It is possible this commit is no longer needed.
         * Check remapped parent's tree id's against the subtree.
         */
        if (found_unnecessary) {
            is_skip_rewrite = 1;

            next_list = remapped_parents;
            while (next_list) {
                if (next_list->item->tree != commit_util->tree) {
                    is_skip_rewrite = 0;
                    break;
                }
                next_list = next_list->next;
            }
        }
    }

    *remapped_parents_ptr = remapped_parents;

    return is_skip_rewrite;
}

/*-----------------------------------------------------------------------------

-----------------------------------------------------------------------------*/

static const char * const builtin_subtree_split_usage[] = {
    "git subtree split [(-P <prefix>)...] [options] <paths>",
    "git subtree split [options] <branch> <paths>",
    "git subtree split [options] [<branch>] -- <paths>",
    NULL,
};

/*-----------------------------------------------------------------------------
TODO: Could split get confused when using 'onto' if the specified onto tree is in
the history multiple times (due to a undo commit)
-----------------------------------------------------------------------------*/
static int cmd_subtree_split(int argc, const char **argv, const char *prefix)
{
    struct split_opts opts;
    struct option options[] = {
        OPT_BOOLEAN(0, "rewrite-head", &opts.rewrite_head, "Rewrite the head to include the generated commit as a subtree merge"),
        OPT_BOOLEAN(0, "rewrite-parents", &opts.rewrite_parents, "Rewrite the commits that are split from to include the generated commit as a subtree merge"), /* TODO: Take an argument as a list of commits to rewrite? */
        OPT_BOOLEAN(0, "rejoin", &opts.rejoin, "Add a merge commit that joins the split out subtree with the source"),
        OPT_BOOLEAN(0, "squash", &opts.squash, "Don't bring in the entire split history"),
        OPT_BOOLEAN(0, "committer", &opts.change_committer, "Rewritten commits will use current commiter information"),
        OPT_CALLBACK(0, "onto", &opts.onto_list, "commit", "Graft the split subtree onto the given commit", opt_string_list),
        OPT_STRING(0, "annotate", &opts.annotation, "annotation", "Add an annotation to the beginning of the commit message of split commits"),
        OPT_STRING(0, "footer", &opts.footer, "annotation", "Add an annotation to the end of the commit message of split commits"),
        OPT_CALLBACK('P', "prefix", &opts.prefix_list, "prefix", "prefix <path>", opt_string_list),
        OPT_END(),
    };
    int i;
    struct commit *rewritten_commit;
    struct commit *commit;
    struct commit_list *interesting_commits = NULL;
    struct commit_list **rewritten_commits = NULL;
    int sz;
    int cnt;
    struct commit* head = NULL;

    /* Parse arguments */
    memset(&opts, 0, sizeof(opts));
    argc = parse_options(argc, argv, prefix, options, builtin_subtree_split_usage, PARSE_OPT_KEEP_UNKNOWN);

    if (opts.squash || opts.rejoin || opts.rewrite_head) {
        if (opts.rewrite_parents || (opts.rewrite_head && (opts.squash || opts.rejoin)))
            die("git subtree split: Can't rewrite and do a squash or a join");

        head = lookup_commit_reference_by_name("HEAD");
        parse_commit(head);
    }

    /*
     * Populate the util with the string length so we're not constantly
     * recomputing and allocate memory for the returned tree SHA1s
     */
    setup_prefix_list(&opts.prefix_list);

    /*
     * Setup the onto list
     */
    onto_list = NULL;
    for (i = 0; i < opts.onto_list.nr; i++)
    {
        int j;
        commit = lookup_commit_reference_by_name(opts.onto_list.items[i].string);
        if (!commit)
            die("git subtree split: unable to resolve onto %s", opts.onto_list.items[i].string);

        parse_commit(commit);
        commit_list_insert(commit, &onto_list);

        /*
         * We don't need to process any of these commits
        */
        for (j = 0; j <= g_details->nr; j++) {
            get_commit_util(commit, j, 1)->referenced = 1;
            get_commit_util(commit, j, 1)->is_subtree = 1;
        }
    }

    /*
     * Get the list of commits that may have subtrees
     */
    interesting_commits = get_interesting_split_commits(argc, argv, prefix);

    debug("\n\n");

    sz = sizeof(*rewritten_commits) * (g_details->nr + 1);
    rewritten_commits = malloc(sz);
    memset(rewritten_commits, 0, sz);

    /*
     * Now that we've collected all of the relevant commits, we'll go through
     * and generate subtree commits for them as needed.
     */
    while ((commit = pop_commit(&interesting_commits)) != NULL) {

        /*
         * Generate the split out commits for each prefix
         */
        for (i = 0; i < g_details->nr; i++) {
            int is_rewrite_needed = 0;
            struct commit_list *remapped_parents = NULL;
            struct commit_util *commit_util;
            struct commit_util *tmp_util;
            struct commit_list *parent;

            debug_commit(commit, i);

            commit_util = get_commit_util(commit, i, 2);
            if (commit_util->referenced || !commit_util->tree) {
                debug("\t\tUninteresting %d\n", commit_util->referenced);
                continue;
            }
            /* 
             * Having a remapping here implies we've already rewritten this
             * commit...and you can only have one subtree per prefix so there
             * can only be zero or one item in this list.
             */
            if (commit_util->remapping) {
                if (commit_util->tree == commit_util->remapping->item->tree) {
                    debug("\t\tAlready split\n");
                    continue;
                }
                else {
                    /*
                     * If we're modifying a split commit, adjust the parentage
                     * of the current split to point to itself.
                     */
                    debug("\t\tAlready split, but changes made\n");
                    is_rewrite_needed = 1;
                    tmp_util = get_commit_util(commit_util->remapping->item, i, 2);
                    commit_list_insert( commit_util->remapping->item, &tmp_util->remapping);
                }
            }

            /*
             * Check this commit's parents and see if the tree id has changed
             */
            parent = commit->parents;
            do {
                struct commit_util *parent_util;
                if (parent) {
                    parent_util = get_commit_util(parent->item, i, 0);
                    debug("\t\tChecking parent %s\n", sha1_to_hex(parent->item->object.sha1));
                    if (parent_util) {
                        struct commit_list *remapping_list = parent_util->remapping;
                        if (parent_util->tree && !remapping_list) {
                            debug("\t\tNON-SUBTREE\n");
                            continue;
                        }
                        if (parent_util->referenced) {
                            /*
                             * This commit is known to not contain subtree.
                             */
                            debug("\t\tREFERENCED\n");
                            continue;
                        }

                        while (remapping_list) {
                            parse_commit(remapping_list->item);
                            if (remapping_list->item->tree == commit_util->tree) {
                                /*
                                 * The tree hasn't changed from this parent
                                 * so we don't need to create a new commit.
                                 * Just map to the already existing split out
                                 * commit.
                                 */
                                commit_list_insert(remapping_list->item, &commit_util->remapping);
                                break;
                            }
                            remapping_list = remapping_list->next;
                        }
                        if (remapping_list) {
                            debug("\t\tFOUND\n");
                            continue;
                        }
                        else {
                            debug("\t\tNOT FOUND\n");
                        }
                    }
                }

                is_rewrite_needed = 1;
            } while (parent != NULL && (parent = parent->next) != NULL);

            if (is_rewrite_needed) {
                struct commit_list *tmp_parent;
                struct commit_list **insert;

                /*
                 * Map the existing parents to their new values
                 */
                insert = &remapped_parents;
                tmp_parent = commit->parents;
                while (tmp_parent) {
                    tmp_util = get_commit_util(tmp_parent->item, i, 0);
                    if (tmp_util && tmp_util->remapping) {
                        struct commit_list *remapping_list = tmp_util->remapping;
                        while (remapping_list) {
                            insert = &commit_list_insert(remapping_list->item, insert)->next;

                            /*
                             * Mark the remapped commit as referenced so we know it
                             * has parents and doesn't need to be displayed.
                             */
                            tmp_util = get_commit_util(remapping_list->item, i, 1);
                            tmp_util->referenced = 1;
                            tmp_util->is_subtree = 1;
                            remapping_list = remapping_list->next;
                        }
                    }
                    tmp_parent = tmp_parent->next;
                }

                /*
                 * Before we create the commit, we need to make sure that all
                 * of its parents contain an interesting commit. This can
                 * happen when a branch that didn't affect the subtree is
                 * merged in to a branch that did affect the subtree.
                 */
                if (cleanup_remapped_parents(commit, i, &remapped_parents)) {
                    is_rewrite_needed = 0;
                }

                if (is_rewrite_needed) {
                    static unsigned int s_created;

                    rewritten_commit = rewrite_commit(commit, commit_util->tree, remapped_parents, 1, &opts);
                    commit_list_insert(rewritten_commit, &commit_util->remapping);
                    commit_util->created = ++s_created;

                    /*
                     * Set the information about the created commit.
                     */
                    tmp_util = get_commit_util(rewritten_commit, i, 1);
                    commit_list_insert(commit, &tmp_util->remapping);
                    tmp_util->created = s_created;
                    tmp_util->is_subtree = 1;

                    debug("\t\t*** CREATED %s\n", sha1_to_hex(rewritten_commit->object.sha1));
                    commit_list_insert(commit, &rewritten_commits[i]);
                }
                else {
                    free_commit_list(remapped_parents);
                }
            }
        }

        /* Rewrite the parent (if requested) */
        if (opts.rewrite_parents) {
            struct commit_util *commit_util;
            struct commit_list *remapped_parents = NULL;
            struct commit_util *tmp_util;
            struct commit_list *tmp_parent;
            struct commit_list **insert;
            int is_changed;

            /*
             * Map the existing parents to their new values
             */
            is_changed = 0;
            insert = &remapped_parents;
            tmp_parent = commit->parents;
            while (tmp_parent) {
                tmp_util = get_commit_util(tmp_parent->item, g_details->nr, 0);
                if (tmp_util && tmp_util->remapping) {
                    struct commit_list *remapping_list = tmp_util->remapping;
                    while (remapping_list) {
                        insert = &commit_list_insert(remapping_list->item, insert)->next;

                        /*
                         * Mark the remapped commit as referenced so we know it
                         * has parents and doesn't need to be displayed.
                         */
                        tmp_util = get_commit_util(remapping_list->item, i, 1);
                        tmp_util->referenced = 1;
                        remapping_list = remapping_list->next;
                        is_changed = 1;
                    }
                }
                else {
                    insert = &commit_list_insert(tmp_parent->item, insert)->next;
                }
                tmp_parent = tmp_parent->next;
            }

            /*
             * Now add any created subtrees
             */
            for (i = 0; i < g_details->nr; i++) {
                tmp_util = get_commit_util(commit, i, 2);
                if (tmp_util->created) {
                    struct commit_list *remapping_list = tmp_util->remapping;
                    while (remapping_list) {
                        insert = &commit_list_insert(remapping_list->item, insert)->next;
                        remapping_list = remapping_list->next;
                    }
                    is_changed = 1;
                }
            }

            if (is_changed) {
                commit_util = get_commit_util(commit, g_details->nr, 2);
                rewritten_commit = rewrite_commit(commit, commit->tree, remapped_parents, 0, &opts);
                commit_list_insert(rewritten_commit, &commit_util->remapping);
                debug("\t*** REWRITE %s\n", sha1_to_hex(rewritten_commit->object.sha1));
                commit_list_insert(commit, &rewritten_commits[g_details->nr]);
            }
            else {
                free_commit_list(remapped_parents);
            }
        }
    }

    interesting_commits = NULL;
    cnt = g_details->nr;
    if (opts.rewrite_parents)
        cnt++;
    for (i = 0; i < cnt; i++) {
        struct commit_list *rewritten;
        struct commit_list *squash_parents = NULL;
        printf("%s\n", i < g_details->nr ? g_details->items[i].prefix : "HEAD");

        rewritten = rewritten_commits[i];
        while (rewritten) {
            struct commit_util *util_rewrite;
            struct commit_util *util_remap;
            struct commit_list *remapping_list;

            /* TODO: Re-verify this with changes for remapping to possibly be more than one commit */
            util_rewrite = get_commit_util(rewritten->item, i, 2);
            remapping_list = util_rewrite->remapping;
            while (remapping_list) {
                if (opts.squash) {
                    struct commit_list *parents = remapping_list->item->parents;
                    while (parents)
                    {
                        util_remap = get_commit_util(parents->item, i, 0);
                        if (!util_remap || !util_remap->created) {
                            debug("\tSquash %s to %s\n", sha1_to_hex(remapping_list->item->object.sha1), sha1_to_hex(parents->item->object.sha1));
                            commit_list_insert(parents->item, &squash_parents);
                        }
                        parents = parents->next;
                    }

                    /* TODO: Optionally take all of the commit messages from these and build them into one? */
                }
                else {
                    util_remap = get_commit_util(remapping_list->item, i, 0);
                    if (!util_remap || (!util_remap->referenced && util_remap->created)) {
                        printf("\t%s\n", sha1_to_hex(remapping_list->item->object.sha1));
                        commit_list_insert(remapping_list->item, &interesting_commits);
                    }
                }

                remapping_list = remapping_list->next;
            }

            rewritten = rewritten->next;
        }

        if (opts.squash) {
            struct commit_util *util;
            util = get_commit_util(head, i, 0);
            commit = create_squash_commit(util->tree, squash_parents, g_details->items[i].prefix);
            commit_list_insert(commit, &interesting_commits);
            printf("\t%s\n", sha1_to_hex(commit->object.sha1));
        }

        free_commit_list(rewritten_commits[i]);
    }
    free(rewritten_commits);

    if (opts.rewrite_head) {
        struct commit_list *parents = NULL;
        struct commit_list **insert = &interesting_commits;
        commit = head;
        parents = head->parents;
        while (parents) {
            /* This will prepend the parents to the list, keeping their original order */
            insert = &commit_list_insert(parents->item, insert)->next;
            parents = parents->next;
        }
        rewritten_commit = rewrite_commit(commit, commit->tree, interesting_commits, 0, &opts);

        printf("%s\n", "HEAD");
        printf("\t%s\n", sha1_to_hex(rewritten_commit->object.sha1));
    }
    else if (opts.rejoin) {
        unsigned char result_commit[20];
        struct strbuf commit_msg = STRBUF_INIT;
        strbuf_addstr(&commit_msg, "Subtree split rejoin\n\n");
        for (i = 0; i < g_details->nr; i++) {
            strbuf_addstr(&commit_msg, g_details->items[i].prefix);
            strbuf_addstr(&commit_msg, "\n");
        }

        commit_list_insert(head, &interesting_commits);

        commit_tree
            (
            commit_msg.buf,
            head->tree->object.sha1,
            interesting_commits,
            result_commit,
            NULL,
            NULL
            );
        strbuf_release(&commit_msg);
        /* Print out in same format as rewrite-parents would */
        printf("%s\n", "HEAD");
        printf("\t%s\n", sha1_to_hex(result_commit));
        update_ref("subtree split", "HEAD", result_commit, NULL, 0, DIE_ON_ERR);
    }
    else {
        free_commit_list(interesting_commits);
    }

    free(g_details);
    return 0;
}

/*---------------------------------------------------------------------------*/
/*                  ######  ####### ######  #     #  #####                   */
/*                  #     # #       #     # #     # #     #                  */
/*                  #     # #       #     # #     # #                        */
/*                  #     # #####   ######  #     # #  ####                  */
/*                  #     # #       #     # #     # #     #                  */
/*                  #     # #       #     # #     # #     #                  */
/*                  ######  ####### ######   #####   #####                   */
/*---------------------------------------------------------------------------*/

static int cmd_subtree_debug(int argc, const char **argv, const char *prefix)
{
    int i;
    const char *split_argv[200];
    struct strbuf **args;
    struct strbuf split_me = STRBUF_INIT;

    //const char *split = "subtree split --rewrite-head";
    //const char *split = "subtree split -P red -P blue --rewrite-head";
    //const char *split = "subtree split local-change-to-subtree -P not-a-subtree -P nested/directory --rewrite-parents";
    //const char *split_resplit = "subtree split split-branch -P not-a-subtree -P nested/directory --rewrite-parents";
    //const char *split_join = "subtree split local-change-to-subtree -P not-a-subtree -P nested/directory --rejoin";
    //const char *split_squash = "subtree split local-change-to-subtree -P red -P blue -P not-a-subtree -P nested/directory --rejoin --squash";
    //const char *split = "subtree split 140dee624e7c29d65543fffaf28cb0dde2c71e48 -P blue -P red --rewrite-parents --always-create";
    //const char *split = "subtree split local-change-to-subtree -P red -P blue --onto 2075c241ce953a8db2b37e0aac731dc60c82a5af --onto b5450a2b82e0072abd37c1791a2bc3810b6e61f0 --footer \nFooter --annotate Split: --always-create";
    //const char *split = "subtree split local-change-to-subtree -P not-a-subtree";
    //const char *split = "subtree split -P indigo --rewrite-head";
    //const char *splitAll = "subtree split -P red -P blue --all --rewrite-parents";
    //const char *add = "subtree add -P indigo --name indigo -r ../indigo";
    //const char *add = "subtree add -P green -r ../green master -n green";
    //const char *merge = "subtree merge -P green olive --squash --name green";
    //const char *merge = "subtree merge -P green green-head --squash";
    const char *merge = "subtree merge -P green -r ../green olive";
    //const char *pull = "subtree pull -P green ../green HEAD";
    //const char *list = "subtree list";

    const char *command = merge;

    strbuf_addstr(&split_me, command);
    args = strbuf_split(&split_me, ' ');
    i = 0;
    while(args[i])
    {
        if(args[i]->buf[args[i]->len-1] == ' ')
            args[i]->buf[args[i]->len-1] = '\0';
        split_argv[i] = args[i]->buf;
        i++;
    }
    split_argv[i] = 0;
    cmd_subtree(i, split_argv, "");
    return 0;


    // rev-list --format=oneline HEAD
    {
        struct commit_list *subtree_commits = NULL;
        struct rev_info rev;
        struct setup_revision_opt opt;
        struct commit *commit;
        static struct string_list prefix_list;

        /*
         * Setup the walk. Make sure the user didn't pass any flags that will
         * mess things up
         */
        init_revisions(&rev, prefix);
        rev.topo_order = 1;
        rev.reverse = 0;
        rev.bisect = 0;
        rev.ignore_merges = 0;
	    rev.max_parents = -1;
	    rev.min_parents = 0;
        
        rev.hide_subtrees = 1;
        string_list_append(&rev.subtrees, "blue");
        string_list_append(&rev.subtrees, "green");
        string_list_append(&rev.subtrees, "orange");
        string_list_append(&rev.subtrees, "red");
        string_list_append(&rev.subtrees, "violet");
        string_list_append(&rev.subtrees, "yellow");
        string_list_append(&rev.subtrees, "end-of-the-rainbow/gold");
        rev.limited = 1;

        memset(&opt, 0, sizeof(opt));
        opt.def = "HEAD";

        {
            const char *filter[] = { "ignored", "HEAD", "--", "end*/go*" };
            setup_revisions(2, filter, &rev, &opt);
        }
        //argc = setup_revisions(argc, argv, &rev, &opt);

        prepare_revision_walk(&rev);
        while ((commit = get_revision(&rev))) {
            printf("%s\n", sha1_to_hex(commit->object.sha1));
        }
    }

    return 0;
}

/*---------------------------------------------------------------------------*/
/*            #####  #     # ######  ####### ######  ####### #######         */
/*           #     # #     # #     #    #    #     # #       #               */
/*           #       #     # #     #    #    #     # #       #               */
/*            #####  #     # ######     #    ######  #####   #####           */
/*                 # #     # #     #    #    #   #   #       #               */
/*           #     # #     # #     #    #    #    #  #       #               */
/*            #####   #####  ######     #    #     # ####### #######         */
/*---------------------------------------------------------------------------*/

static const char * const builtin_subtree_usage[] = {
    "git subtree add"
    "git subtree list",
    "git subtree merge",
    "git subtree pull",
    "git subtree push",
    "git subtree reset", /* TODO */
    "git subtree split",
    "git subtree squash", /* TODO */
    NULL
};

int cmd_subtree(int argc, const char **argv, const char *prefix)
{
    struct option options[] = {
        OPT_END()
    };

    int result;

    argc = parse_options(argc, argv, prefix, options, builtin_subtree_usage, PARSE_OPT_STOP_AT_NON_OPTION);

    if (argc < 1)
        usage_with_options(builtin_subtree_usage, options);
    else if (!strcmp(argv[0], "add"))
        result = cmd_subtree_add(argc, argv, prefix);
    else if (!strcmp(argv[0], "list"))
        result = cmd_subtree_list(argc, argv, prefix);
    else if (!strcmp(argv[0], "merge"))
        result = cmd_subtree_merge(argc, argv, prefix);
    else if (!strcmp(argv[0], "pull"))
        result = cmd_subtree_pull(argc, argv, prefix);
    else if (!strcmp(argv[0], "push"))
        result = cmd_subtree_push(argc, argv, prefix);
    else if (!strcmp(argv[0], "split"))
        result = cmd_subtree_split(argc, argv, prefix);
    else if (!strcmp(argv[0], "debug"))
        result = cmd_subtree_debug(argc, argv, prefix); /* TODO: Remove */
    else {
        error("Unknown subcommand: %s", argv[0]);
        usage_with_options(builtin_subtree_usage, options);
    }

    return 0;
}

/*-----------------------------------------------------------------------------
                          http://patorjk.com/software/taag/
                                      BANNER
                            #     #####   #####  ### ###
                           # #   #     # #     #  #   #
                          #   #  #       #        #   #
                         #     #  #####  #        #   #
                         #######       # #        #   #
                         #     # #     # #     #  #   #
                         #     #  #####   #####  ### ###
-----------------------------------------------------------------------------*/

/*-----------------------------------------------------------------------------
                       ####### ####### ######  #######
                          #    #     # #     # #     #
                          #    #     # #     # #     #
                          #    #     # #     # #     #
                          #    #     # #     # #     #
                          #    #     # #     # #     #
                          #    ####### ######  #######
 -----------------------------------------------------------------------------
* Figure out how to make tab auto complete find branch names, remotes, etc.
* Add subtree reset command?
* Add options to reflog to ignore subtrees
* Add push. Figure out how to push split out changes when split from a merge
   commit
* Detailed squash commit messages
* Figure out how to present split --all (or any split that includes multiple
   branches
* Subtree cherry-pick command?
* Example post-commit hook that rewrites head to split subtrees
* Passing an invalid remote (or remote branch) to add doesn't cleanup
   index.lock
* On add, detect existing subtree and (optionally?) do a merge with it
* Allow them to specify names instead of prefixes (to lookup prefix from 
   .subtree)
* Pull down subtree refs/ into refs/subtree/<name>/*
* Wildcard searches for prefixes in list & rev-list? This could be hard 
   since a lot of the logic assumes 1 subtree per prefix.
-----------------------------------------------------------------------------*/
