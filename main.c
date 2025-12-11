#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TARGET_DIR "."
#define TEMP_FILE_TEMPLATE "cbr_file_XXXXXX"

#define BOLD "\x1b[1m"
#define RED "\x1b[31m"
#define GREEN "\x1b[32m"
#define RESET "\x1b[0m"

// ===== PROTOTYPES ============================================================

int str_cmp(const void *a, const void *b);

// ===== DATA STRUCTURES =======================================================

typedef struct {
    char **files;
    int capacity;
    int count;
} FileList;

#define FL_INIT {.files = NULL, .capacity = 0, .count = 0}

void file_list_add(FileList *fl, char *file_name) {
    if (!fl->files) {
        fl->files = malloc(sizeof(char **));
        fl->files[0] = strdup(file_name);
        fl->capacity = 1;
        fl->count = 1;
    } else {
        if (fl->count >= fl->capacity) {
            fl->capacity *= 2;
            fl->files = realloc(fl->files, fl->capacity * sizeof(char **));
        }
        fl->files[fl->count] = strdup(file_name);
        fl->count++;
    }
}

bool file_list_has_entry(FileList *fl, char *file_name) {
    char **match =
        bsearch(&file_name, fl->files, fl->count, sizeof(char **), str_cmp);
    return match != NULL;
}

void file_list_delete(FileList *fl) {
    for (int i = 0; i < fl->count; i++) {
        free(fl->files[i]);
    }
    if (fl->files) { free(fl->files); }
}

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

void rename_file(const char *old_filename, const char *new_filename) {
    int result = rename(old_filename, new_filename);
    if (result != 0) {
        perror("rename");
        exit(EXIT_FAILURE);
    }

    printf(BOLD GREEN "Renamed " RESET "'%s'\n", old_filename);
    printf(GREEN "     ->" RESET " '%s'\n", new_filename);
}

void remove_file(const char *filename) {
    if (!file_exists(filename)) {
        printf("Error: File '%s' does not exist.\n", filename);
        exit(EXIT_FAILURE);
    }
    remove(filename);
    printf(BOLD RED "Removed " RESET "'%s'\n", filename);
}

// ===== MAIN ==================================================================

int main(void) {
    FileList initial_file_list = FL_INIT;
    FileList new_file_list = FL_INIT;
    FileList new_sorted_file_list = FL_INIT;

    FileList temp_file_list = FL_INIT;
    FileList temp_new_file_list = FL_INIT;

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
            file_list_add(&initial_file_list, entry->d_name);
        }
    }

    // check that there is at least one input filename
    if (initial_file_list.count == 0) { return EXIT_SUCCESS; }

    // sort file names
    qsort(initial_file_list.files, initial_file_list.count, sizeof(char **),
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
    for (int i = 0; i < initial_file_list.count; i++) {
        fprintf(tmp_file_ptr, "%s\n", initial_file_list.files[i]);
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
        file_list_add(&new_file_list, buffer);
    }

    // check that there are same number of lines
    if (initial_file_list.count != new_file_list.count) {
        printf("Error: Mismatched number of lines. New filename list contains "
               "%d entries while original list contains %d.\n",
               new_file_list.count, initial_file_list.count);
        return EXIT_FAILURE;
    }

    // shallow copy new file list for sorting
    new_sorted_file_list.files = malloc(new_file_list.count * sizeof(char **));
    memcpy(new_sorted_file_list.files, new_file_list.files,
           new_file_list.count * sizeof(char **));
    new_sorted_file_list.count = new_file_list.count;

    qsort(new_sorted_file_list.files, new_sorted_file_list.count,
          sizeof(char **), str_cmp);

    // further validation
    for (int i = 0; i < initial_file_list.count; i++) {
        char *new_file_name = new_file_list.files[i];

        // skip files to be deleted
        if (new_file_name[0] == '#') { continue; }

        // check that output filenames do not match 'outside' file
        if (!file_list_has_entry(&initial_file_list, new_file_name)) {
            if (file_exists(new_file_name)) {
                printf("Error: File '%s' already exists.\n", new_file_name);
                return EXIT_FAILURE;
            }
        }

        // check that output filenames are unique
        if (i == initial_file_list.count - 1) { continue; }

        if (strcmp(new_sorted_file_list.files[i],
                   new_sorted_file_list.files[i + 1]) == 0) {
            printf("Error: Output filenames are not unique ('%s').\n",
                   new_sorted_file_list.files[i]);
            return EXIT_FAILURE;
        }
    }

    // rename/delete files
    for (int i = 0; i < initial_file_list.count; i++) {
        char *initial_file_name = initial_file_list.files[i];
        char *new_file_name = new_file_list.files[i];

        // skip if unchanged
        if (strcmp(initial_file_name, new_file_name) == 0) { continue; }

        // delete
        if (new_file_name[0] == '#') {
            remove_file(initial_file_name);
            continue;
        }

        // if instance of cyclic renaming
        if (file_list_has_entry(&initial_file_list, new_file_name)) {
            char temp_filename[256];
            generate_unique_filename(temp_filename, sizeof(temp_filename));

            // rename later from temp name to avoid conflict
            rename_file(initial_file_name, temp_filename);
            file_list_add(&temp_file_list, temp_filename);
            file_list_add(&temp_new_file_list, new_file_name);
        } else {
            rename_file(initial_file_name, new_file_name);
        }
    }

    for (int i = 0; i < temp_file_list.count; i++) {
        char *initial_file_name = temp_file_list.files[i];
        char *new_file_name = temp_new_file_list.files[i];

        rename_file(initial_file_name, new_file_name);
    }

    // file_list_delete(&initial_file_list);
    // file_list_delete(&new_file_list);
    // file_list_delete(&new_sorted_file_list);
    // file_list_delete(&temp_file_list);
    // file_list_delete(&temp_new_file_list);

    fclose(tmp_file_ptr);
    remove(tmp_file_path);

    return EXIT_SUCCESS;
}
