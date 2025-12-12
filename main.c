#include <argp.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CBR_VERSION "0.1"

// used to define type-safe dynamic arrays
#define DEFINE_ARRAY_TYPE(Name, Type)                                          \
    typedef struct {                                                           \
        Type *data;                                                            \
        int capacity;                                                          \
        int count;                                                             \
    } Name;                                                                    \
                                                                               \
    static inline void Name##_init(Name *a) {                                  \
        a->data = NULL;                                                        \
        a->capacity = 0;                                                       \
        a->count = 0;                                                          \
    }                                                                          \
                                                                               \
    static inline void Name##_add(Name *a, Type item) {                        \
        if (a->count >= a->capacity) {                                         \
            a->capacity = a->capacity ? a->capacity * 2 : 1;                   \
            a->data = realloc(a->data, a->capacity * sizeof(Type));            \
        }                                                                      \
        a->data[a->count] = item;                                              \
        a->count++;                                                            \
    }                                                                          \
                                                                               \
    static inline void Name##_free(Name *a) {                                  \
        for (int i = 0; i < a->count; i++) {                                   \
            free(a->data[i]);                                                  \
        }                                                                      \
        if (a->data) { free(a->data); }                                        \
    }

#define BOLD "\x1b[1m"
#define RED "\x1b[31m"
#define GREEN "\x1b[32m"
#define YELLOW "\x1b[33m"
#define RESET "\x1b[0m"

#define TARGET_DIR "."

// ===== DATA STRUCTURES =======================================================

// used for multi-step file renaming
typedef struct {
    char *initial_name;
    char *temp_name;
    char *new_name;
} RenamePath;

DEFINE_ARRAY_TYPE(FilenameList, char *)
DEFINE_ARRAY_TYPE(RenamePathList, RenamePath *)

typedef struct {
    bool force;          // whether to overwrite existing files
    bool silent;         // whether to write to stdout
    bool trash;          // whether to marked files to trash
    char delete_char;    // character used to mark file for deletion
    char *editor;        // specify editor to use
    FilenameList *files; // the files to be renamed (args)
} Arguments;

// ===== ARGUMENT PARSING ======================================================

const char *argp_program_version = "v" CBR_VERSION;

static char doc[] = "cbr -- Bulk renaming utility";

static char args_doc[] = "[FILE]...";

static struct argp_option options[] = {
    {"delchar", 'd', "CHARACTER", 0,
     "Specify what deletion mark to use. Default '#'", 0},
    {"editor", 'e', "PROGRAM", 0, "Specify what editor to use", 0},
    {"force", 'f', 0, 0, "Allow overwriting of existing files", 0},
    {"silent", 's', 0, 0, "Only report errors", 0},
    {"trash", 't', 0, 0, "Send files to trash instead of deleting them.", 0},
    {0}};

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
    Arguments *arguments = state->input;

    switch (key) {
    case 'd':
        arguments->delete_char = arg[0];
        break;
    case 'e':
        arguments->editor = arg;
        break;
    case 'f':
        arguments->force = true;
        break;
    case 's':
        arguments->silent = true;
        break;
    case 't':
        arguments->trash = true;
        break;
    case ARGP_KEY_ARG:
        FilenameList_add(arguments->files, strdup(arg));
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = {options, parse_opt, args_doc, doc, 0, 0, 0};

// ===== UTIL ==================================================================

bool file_exists(const char *filename) {
    struct stat st;
    return lstat(filename, &st) == 0; // lstat() does not follow symlink
}

// whether binary exists in directory in $PATH
bool binary_exists(const char *name) {
    const char *path = getenv("PATH");
    if (!path) { return false; }

    char *paths = strdup(path); // cannot directly edit str from getenv()
    if (!paths) { return false; }

    char *dir = strtok(paths, ":");
    while (dir) {
        char absolute_path[512];
        snprintf(absolute_path, sizeof(absolute_path), "%s/%s", dir, name);

        if (file_exists(absolute_path)) {
            free(paths);
            return true;
        }

        dir = strtok(NULL, ":");
    }

    free(paths);
    return false;
}

void generate_unique_filepath(char buffer[], int buf_len, char *prefix) {
    do {
        snprintf(buffer, buf_len, "%s_%d", prefix, rand() % 1000);
    } while (file_exists(buffer));
}

char *get_editor_from_env(void) {
    char *editor_path;

    editor_path = getenv("VISUAL");
    if (editor_path) { return editor_path; }

    editor_path = getenv("EDITOR");
    if (editor_path) { return editor_path; }

    if (binary_exists("nano")) { return "nano"; }
    if (binary_exists("vi")) { return "vi"; }

    return NULL;
}

int str_cmp(const void *a, const void *b) {
    const char *sa = *(const char **)a;
    const char *sb = *(const char **)b;
    return strcmp(sa, sb);
}

// filename list must be sorted
bool filename_list_has(FilenameList *fl, char *filename) {
    char **match =
        bsearch(&filename, fl->data, fl->count, sizeof(char **), str_cmp);
    return match != NULL;
}

bool rename_file(const char *old_filename, const char *new_filename) {
    int result = rename(old_filename, new_filename);
    if (result != 0) {
        perror("rename");
        fprintf(stderr, "Error: Could not rename '%s' to '%s'\n", old_filename,
                new_filename);
        return false;
    }
    return true;
}

// https://www.csl.mtu.edu/cs4411.ck/www/NOTES/process/fork/create.html
// returns whether successful
bool gio_trash(char *argv[]) {
    pid_t pid = fork();

    // if fork() unsuccesful
    if (pid < 0) {
        perror("fork");
        return false;
    }

    // code for child process
    if (pid == 0) {
        execvp("gio", argv);

        // if execvp returns, it's an error
        perror("execvp");

        // POSIX convention for failed exec() in child-process
        // 127 means "exec failed for this command"
        // _exit() bypasses standard exit() procedure (skips atexit() handlers)
        _exit(127);
    }

    // in parent process, wait for child
    int status;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return false;
    }

    // if child process terminated normalled and return code is 0
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) { return true; }

    return false;
}

void print_rename_message(const char *old_filename, const char *new_filename) {
    printf(BOLD GREEN "Renamed " RESET "'%s'\n", old_filename);
    printf(GREEN "     ->" RESET " '%s'\n", new_filename);
}

void print_delete_message(const char *filename) {
    printf(BOLD RED "Removed " RESET "'%s'\n", filename);
}

void print_trash_message(const char *filename) {
    printf(BOLD YELLOW "Trashed " RESET "'%s'\n", filename);
}

// ===== MAIN ==================================================================

int main(int argc, char *argv[]) {
    FilenameList initial_names_list, new_names_list, new_sorted_names_list;
    FilenameList_init(&initial_names_list);
    FilenameList_init(&new_names_list);
    FilenameList_init(&new_sorted_names_list);

    // used if -t/--trash is specified
    FilenameList trash_list;
    FilenameList_init(&trash_list);

    // used to handle temporary files
    RenamePathList rename_path_list;
    RenamePathList_init(&rename_path_list);

    // default arguments
    Arguments arguments = {.delete_char = '#',
                           .editor = NULL,
                           .force = false,
                           .silent = false,
                           .trash = false,
                           .files = &initial_names_list};

    // parse arguments
    argp_parse(&argp, argc, argv, 0, 0, &arguments);

    // check that `gio` is available if trashing files
    if (arguments.trash) {
        if (!binary_exists("gio")) {
            fprintf(stderr, "Error: gio (as part of GLib) is required for "
                            "trash functionality.\n");
            goto fail;
        }
    }

    DIR *cur_dir = NULL;
    FILE *tmp_edit_file = NULL;

    // if no file arguments specified, populate input list with contents of
    // current working directory
    if (initial_names_list.count == 0) {
        cur_dir = opendir(TARGET_DIR);
        if (!cur_dir) {
            perror("opendir");
            goto fail;
        }

        struct dirent *entry;

        // collect names of regular files and symbolic links
        while ((entry = readdir(cur_dir)) != NULL) {
            if (entry->d_type == DT_REG || entry->d_type == DT_LNK) {
                if (entry->d_name[0] == arguments.delete_char) {
                    fprintf(stderr,
                            "Error: Input filenames ('%s') cannot begin with "
                            "delete character '%c'.\n",
                            entry->d_name, arguments.delete_char);
                    goto fail;
                }
                FilenameList_add(&initial_names_list, strdup(entry->d_name));
            }
        }

        closedir(cur_dir);
        cur_dir = NULL;
    }

    // check that there is at least one input filename
    if (initial_names_list.count == 0) { exit(EXIT_SUCCESS); }

    // check that input files exist
    for (int i = 0; i < initial_names_list.count; i++) {
        if (!file_exists(initial_names_list.data[i])) {
            fprintf(stderr, "Error: File '%s' does not exist.\n",
                    initial_names_list.data[i]);
            goto fail;
        }
    }

    // sort file names
    qsort(initial_names_list.data, initial_names_list.count, sizeof(char **),
          str_cmp);

    // temp file creation
    char tmp_file_path[32];
    generate_unique_filepath(tmp_file_path, sizeof(tmp_file_path),
                             "/tmp/cbr_edit_file");

    // open temp file
    tmp_edit_file = fopen(tmp_file_path, "w+");
    if (!tmp_edit_file) {
        perror("fopen");
        goto fail;
    }

    // write to temp file
    for (int i = 0; i < initial_names_list.count; i++) {
        fprintf(tmp_edit_file, "%s\n", initial_names_list.data[i]);
    }

    fclose(tmp_edit_file);
    tmp_edit_file = NULL;

    // edit file list
    char *editor = arguments.editor;
    if (!editor) { editor = get_editor_from_env(); }
    if (!editor) {
        fprintf(stderr, "Error: Could not find any editor from environment.\n");
        goto fail;
    }

    char edit_cmd[256];
    snprintf(edit_cmd, sizeof(edit_cmd), "%s %s", editor, tmp_file_path);
    int return_code = system(edit_cmd);

    if (return_code != 0) {
        fprintf(stderr, "Error: Editor returned exit code %d.\n", return_code);
        goto fail;
    }

    // open edited temp file
    tmp_edit_file = fopen(tmp_file_path, "r");
    if (!tmp_edit_file) {
        perror("fopen");
        goto fail;
    }

    // read temp file
    char new_fn_buf[256];
    while (fgets(new_fn_buf, sizeof(new_fn_buf), tmp_edit_file)) {
        new_fn_buf[strcspn(new_fn_buf, "\n")] = '\0'; // strip newline
        FilenameList_add(&new_names_list, strdup(new_fn_buf));
    }

    // check that there are same number of lines
    if (initial_names_list.count != new_names_list.count) {
        fprintf(stderr,
                "Error: Mismatched number of lines. New filename list contains "
                "%d entries while original list contains %d.\n",
                new_names_list.count, initial_names_list.count);
        goto fail;
    }

    // shallow copy new file list for sorting
    new_sorted_names_list.data = malloc(new_names_list.count * sizeof(char **));
    memcpy(new_sorted_names_list.data, new_names_list.data,
           new_names_list.count * sizeof(char **));
    new_sorted_names_list.count = new_names_list.count;

    qsort(new_sorted_names_list.data, new_sorted_names_list.count,
          sizeof(char **), str_cmp);

    // further validation
    for (int i = 0; i < initial_names_list.count; i++) {
        char *new_filename = new_names_list.data[i];

        // skip files to be deleted
        if (new_filename[0] == arguments.delete_char) { continue; }

        // if renaming to filename not in input list and file already exists
        if (!filename_list_has(&initial_names_list, new_filename)) {
            if (!arguments.force && file_exists(new_filename)) {
                fprintf(stderr, "Error: File '%s' already exists.\n",
                        new_filename);
                goto fail;
            }
        }

        // check that output filenames are unique
        if (i == initial_names_list.count - 1) { continue; }

        if (strcmp(new_sorted_names_list.data[i],
                   new_sorted_names_list.data[i + 1]) == 0) {
            fprintf(stderr, "Error: Output filenames are not unique ('%s').\n",
                    new_sorted_names_list.data[i]);
            goto fail;
        }
    }

    // rename/delete files
    for (int i = 0; i < initial_names_list.count; i++) {
        char *initial_filename = initial_names_list.data[i];
        char *new_filename = new_names_list.data[i];

        // skip if unchanged
        if (strcmp(initial_filename, new_filename) == 0) { continue; }

        // if marked for deletion
        if (new_filename[0] == arguments.delete_char) {
            // trash
            if (arguments.trash) {
                FilenameList_add(&trash_list, strdup(initial_filename));
                continue;
            }
            // delete
            else {
                int result = remove(initial_filename);
                if (result != 0) {
                    perror("remove");
                    fprintf(stderr, "Error: Could not delete file '%s'.\n",
                            initial_filename);
                    goto fail;
                }
                if (!arguments.silent) {
                    print_delete_message(initial_filename);
                }
                continue;
            }
        }

        // if instance of cyclic renaming
        if (filename_list_has(&initial_names_list, new_filename)) {
            char temp_filename[256];
            generate_unique_filepath(temp_filename, sizeof(temp_filename),
                                     "cbr_transition_file");

            // will rename later from temp name to avoid conflict
            bool success = rename_file(initial_filename, temp_filename);
            if (!success) { goto fail; }

            RenamePath *rp = malloc(sizeof *rp);
            *rp = (RenamePath){.initial_name = initial_filename,
                               .temp_name = strdup(temp_filename),
                               .new_name = new_filename};
            RenamePathList_add(&rename_path_list, rp);
        }
        // else standard rename
        else {
            bool success = rename_file(initial_filename, new_filename);
            if (!success) { goto fail; }
            if (!arguments.silent) {
                print_rename_message(initial_filename, new_filename);
            }
        }
    }

    // trash files in batches
    if (trash_list.count > 0) {
        // first argument is program itself, second is trash command
        char *gio_args[200] = {"gio", "trash"};
        int args_idx = 2;

        for (int i = 0; i < trash_list.count; i++) {
            gio_args[args_idx] = trash_list.data[i];
            args_idx++;

            if (args_idx == (sizeof(gio_args) / sizeof(gio_args[0])) - 1) {
                gio_args[args_idx] = NULL;
                bool success = gio_trash(gio_args);
                if (!success) { goto fail; }
                args_idx = 2;
            }

            print_trash_message(trash_list.data[i]);
        }

        // flush buffer
        if (args_idx > 3) {
            gio_args[args_idx] = NULL;
            bool success = gio_trash(gio_args);
            if (!success) { goto fail; }
        }
    }

    // rename temp files to new filenames
    for (int i = 0; i < rename_path_list.count; i++) {
        RenamePath *rp = rename_path_list.data[i];
        bool success = rename_file(rp->temp_name, rp->new_name);
        if (!success) { goto fail; }
        if (!arguments.silent) {
            print_rename_message(rp->initial_name, rp->new_name);
        }
    }

    // cleanup
    FilenameList_free(&initial_names_list);
    FilenameList_free(&new_names_list);
    FilenameList_free(&trash_list);
    RenamePathList_free(&rename_path_list);
    free(new_sorted_names_list.data);

    fclose(tmp_edit_file);
    remove(tmp_file_path);

    return EXIT_SUCCESS;

fail:
    FilenameList_free(&initial_names_list);
    FilenameList_free(&new_names_list);
    FilenameList_free(&trash_list);
    RenamePathList_free(&rename_path_list);
    if (new_sorted_names_list.data) { free(new_sorted_names_list.data); }

    if (cur_dir) { closedir(cur_dir); }
    if (tmp_edit_file) { fclose(tmp_edit_file); }
    if (file_exists(tmp_file_path)) { remove(tmp_file_path); }

    return EXIT_FAILURE;
}
