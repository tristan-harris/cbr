#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    }

#define BOLD "\x1b[1m"
#define RED "\x1b[31m"
#define GREEN "\x1b[32m"
#define RESET "\x1b[0m"

#define TARGET_DIR "."
#define TEMP_FILE_TEMPLATE "cbr_file_XXXXXX"

// ===== DATA STRUCTURES =======================================================

// used for multi-step file renaming (cyclic renaming)
typedef struct {
    char *initial_name;
    char *temp_name;
    char *new_name;
} RenamePath;

DEFINE_ARRAY_TYPE(FilenameList, char *);
DEFINE_ARRAY_TYPE(RenamePathList, RenamePath);

// ===== UTIL ==================================================================

// whether binary exists in directory in $PATH
bool binary_exists(const char *name) {
    const char *path = getenv("PATH");
    if (!path) { return 0; }

    char *paths = strdup(path); // cannot directly edit str from getenv()
    if (!paths) { return 0; }

    char *dir = strtok(paths, ":");
    while (dir) {
        char absolute_path[512];
        snprintf(absolute_path, sizeof(absolute_path), "%s/%s", dir, name);

        if (access(absolute_path, X_OK) == 0) {
            free(paths);
            return true;
        }

        dir = strtok(NULL, ":");
    }

    free(paths);
    return false;
}

bool file_exists(const char *filename) {
    struct stat st;
    return lstat(filename, &st) == 0; // lstat() does not follow symlink
}

void generate_unique_filename(char buffer[], int buf_len) {
    do {
        snprintf(buffer, buf_len, "cbr_transition_file_%d", rand() % 1000);
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

void rename_file(const char *old_filename, const char *new_filename) {
    int result = rename(old_filename, new_filename);
    if (result != 0) {
        perror("rename");
        printf("Attempted to rename '%s' to '%s'\n", old_filename,
               new_filename);
        exit(EXIT_FAILURE);
    }
}

void remove_file(const char *filename) {
    if (!file_exists(filename)) {
        printf("Error: File '%s' does not exist.\n", filename);
        exit(EXIT_FAILURE);
    }
    remove(filename);
}

void print_rename_message(const char *old_filename, const char *new_filename) {
    printf(BOLD GREEN "Renamed " RESET "'%s'\n", old_filename);
    printf(GREEN "     ->" RESET " '%s'\n", new_filename);
}

void print_delete_message(const char *filename) {
    printf(BOLD RED "Removed " RESET "'%s'\n", filename);
}

// ===== MAIN ==================================================================

int main(void) {
    FilenameList initial_names_list, new_names_list, new_sorted_names_list;
    FilenameList_init(&initial_names_list);
    FilenameList_init(&new_names_list);

    // used to handle temporary files
    RenamePathList rename_path_list;
    RenamePathList_init(&rename_path_list);

    // read dir
    DIR *dir = opendir(TARGET_DIR);
    if (!dir) {
        perror("opendir");
        return EXIT_FAILURE;
    }

    struct dirent *entry;

    // collect names of regular files and symbolic links
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG || entry->d_type == DT_LNK) {
            if (entry->d_name[0] == '#') {
                printf("Error: Input filenames ('%s') cannot begin with '#'.\n",
                       entry->d_name);
                return EXIT_FAILURE;
            }
            FilenameList_add(&initial_names_list, strdup(entry->d_name));
        }
    }

    // check that there is at least one input filename
    if (initial_names_list.count == 0) { return EXIT_SUCCESS; }

    // sort file names
    qsort(initial_names_list.data, initial_names_list.count, sizeof(char **),
          str_cmp);

    // temp file creation
    char tmp_file_path[] = TEMP_FILE_TEMPLATE;
    int tmp_file_fd = mkstemp(tmp_file_path); // TODO: is file already open?

    if (tmp_file_fd == -1) {
        perror("mkstemp");
        return EXIT_FAILURE;
    }

    // open temp file
    FILE *tmp_file_ptr = fdopen(tmp_file_fd, "w");
    if (!tmp_file_ptr) {
        perror("fdopen");
        return EXIT_FAILURE;
    }

    // write to temp file
    for (int i = 0; i < initial_names_list.count; i++) {
        fprintf(tmp_file_ptr, "%s\n", initial_names_list.data[i]);
    }

    closedir(dir);
    fclose(tmp_file_ptr);

    // edit file list
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s %s", get_editor_from_env(), tmp_file_path);
    int return_code = system(cmd);

    if (return_code != 0) {
        printf("Error: Editor returned failure code %d.\n", return_code);
        return EXIT_FAILURE;
    }

    // open edited temp file
    tmp_file_ptr = fopen(tmp_file_path, "r");
    if (!tmp_file_ptr) {
        perror("fopen");
        return EXIT_FAILURE;
    }

    // read temp file
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), tmp_file_ptr)) {
        buffer[strcspn(buffer, "\n")] = '\0'; // strip newline
        FilenameList_add(&new_names_list, strdup(buffer));
    }

    // check that there are same number of lines
    if (initial_names_list.count != new_names_list.count) {
        printf("Error: Mismatched number of lines. New filename list contains "
               "%d entries while original list contains %d.\n",
               new_names_list.count, initial_names_list.count);
        return EXIT_FAILURE;
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
        if (new_filename[0] == '#') { continue; }

        // if renaming to filename not in input list and file already exists
        if (!filename_list_has(&initial_names_list, new_filename)) {
            if (file_exists(new_filename)) {
                printf("Error: File '%s' already exists.\n", new_filename);
                return EXIT_FAILURE;
            }
        }

        // check that output filenames are unique
        if (i == initial_names_list.count - 1) { continue; }

        if (strcmp(new_sorted_names_list.data[i],
                   new_sorted_names_list.data[i + 1]) == 0) {
            printf("Error: Output filenames are not unique ('%s').\n",
                   new_sorted_names_list.data[i]);
            return EXIT_FAILURE;
        }
    }

    // rename/delete files
    for (int i = 0; i < initial_names_list.count; i++) {
        char *initial_filename = initial_names_list.data[i];
        char *new_filename = new_names_list.data[i];

        // skip if unchanged
        if (strcmp(initial_filename, new_filename) == 0) { continue; }

        // delete
        if (new_filename[0] == '#') {
            remove_file(initial_filename);
            print_delete_message(initial_filename);
            continue;
        }

        // if instance of cyclic renaming
        if (filename_list_has(&initial_names_list, new_filename)) {
            char temp_filename[256];
            generate_unique_filename(temp_filename, sizeof(temp_filename));

            // rename later from temp name to avoid conflict
            rename_file(initial_filename, temp_filename);

            RenamePath rp = {.initial_name = initial_filename,
                             .temp_name = strdup(temp_filename),
                             .new_name = new_filename};
            RenamePathList_add(&rename_path_list, rp);

        } else {
            rename_file(initial_filename, new_filename);
            print_rename_message(initial_filename, new_filename);
        }
    }

    for (int i = 0; i < rename_path_list.count; i++) {
        RenamePath *rp = &rename_path_list.data[i];
        rename_file(rp->temp_name, rp->new_name);
        print_rename_message(rp->initial_name, rp->new_name);
    }

    // free memory from filename lists
    for (int i = 0; i < initial_names_list.count; i++) {
        free(initial_names_list.data[i]);
    }
    free(initial_names_list.data);

    for (int i = 0; i < new_names_list.count; i++) {
        free(new_names_list.data[i]);
    }
    free(new_names_list.data);

    free(new_sorted_names_list.data);

    fclose(tmp_file_ptr);
    remove(tmp_file_path);

    return EXIT_SUCCESS;
}
