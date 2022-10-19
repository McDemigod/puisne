#include "libc/calls/calls.h"
#include "libc/calls/mount.h"
#include "libc/calls/struct/stat.h"
#include "libc/calls/struct/utsname.h"
#include "libc/dce.h"
#include "libc/fmt/conv.h"
#include "libc/mem/mem.h"
#include "libc/runtime/runtime.h"
#include "libc/stdio/temp.h"
#include "libc/sysv/consts/clone.h"
#include "libc/sysv/consts/mount.h"
#include "libc/sysv/consts/nr.h"
#include "libc/time/struct/tm.h"
#include "libc/time/time.h"
#include "libc/x/x.h"
#include "libc/x/xasprintf.h"
#include "libc/zip.h"
#include "libc/zipos/zipos.internal.h"
#include "third_party/getopt/getopt.h"
#include "tool/args/args.h"

// Some constants for mounting
// per `man 7 user_namespaces`
//   Overlayfs needs 5.11
//   5.12 adds a `uid_map` rule to address a security issue.
#define LINUX_KERNEL_MAJOR_MIN 5
#define LINUX_KERNEL_MINOR_MIN 12
#define LINUX_KERNEL_PATCH_MIN 0
#define CLONE_NEWUSER          0x10000000 /* New user namespace */

#define APP_SUFFIX ".app"

// Parameters (& defaults, if determinate):
static char tidy_mode;      // -m: mount, -n: none
                            //   if running Linux & kernel >= 5.12.0: mount
                            //   else: none
static char overlay = 'o';  // -o [over], under
static char unzip = 'u';    // -u [update], all, new, existing, freshen, none
static char* unzip_dir;     // -d directory; default differs by mode.
                            //   mount: `dirname(argv[0])/.puisne/name.app`
                            //   none: `dirname(argv[0])`
static char* work_dir;      // -w directory; defaults to some tempdir.
                            //    Exposed in case that is on a different volume.

// Globals
static char* name;            // Name of the package
static char* invocation_dir;  // Where the PUISNE executable appears.

char** files = 0;         // Array of filenames in zip object store,
int64_t* timestamps = 0;  // timestamps for same,
int* modes = 0;           // & permissions &c.

void split_args(int* argc, char*** argv, int* package_argc,
                char*** package_argv) {
  /*
      Splits PUISNE args from args intended for the package.
      Generally, command line args are passed to the package (ie. ignored by
      PUISNE); Only if the first arg is `--` does PUISNE begin to process them.
      Any subsequent `--` means further options are passed to the package.
      Splitting these now means `LoadZipArgs` won't intercept package args.
  */

  if ((*argc > 1)) {  // We have something from the command line...
    if (strcmp((*argv)[1], "--") == 0) {  // ...for PUISNE:
      // The quickest way to do this is to "shift" the beginning of argv
      // up by 1, overwritting `--` with argv[0]. This leaves a dangling
      // value before that, but that should be trivial.
      (*argv)[1] = (*argv)[0];
      *argv += 1;
      *argc -= 1;
      // find the next `--`, if it exists
      for (int i = 0; i < *argc; i++) {
        if (strcmp((*argv)[i], "--") == 0) {
          // Anything after it would be for the package (& see below).
          *package_argv = *argv + i + 1;
          *package_argc = *argc - i - 1;
          *argc = i;
          break;
        }
      }
    } else {
      // ...for the package:
      *package_argv = *argv + 1;
      *package_argc = *argc - 1;
      *argc = 1;
    }
  }
}

void fix_path(char** path) {
  /*
      Corrects paths, if necessary.
      Generally handled by the shell, but will be necessary if eg. `~` is used
      within `.args`.
      Also should work in windows.
  */

  if (((*path)[0] == '~') && !isdirectory("~")) {
    if (IsWindows()) {
      *path = xstrcat(getenv("userprofile"), (*path) + 1);
    } else {
      *path = replaceuser(*path);
    }
  }
}

void print_help(void) {
  /*
      Prints PUISNE help file or an error; exit either way.
  */

  FILE* fi;
  unsigned char buffer[BUFSIZ];
  fi = fopen("/zip/puisne/help.txt", "r");
  if (!fi) {
    fprintf(stderr, "PUISNE: Zip error reading help file!\n");
    exit(1);
  }

  int count;
  while (count = fread(buffer, sizeof(buffer[0]), BUFSIZ, fi)) {
    fwrite(buffer, sizeof(buffer[0]), count, stdout);
  }
  fclose(fi);
  exit(0);
}

void print_empty(void) {
  /*
      Displays additional information if no package is found, then the help.
      Basically, coming out of nowhere be informative even without any
      leading `--` shenanigans.
  */
  fprintf(
      stdout,
      "This is an empty PUISNE.\n"
      "Add an application folder to make this a self-contained bundle, eg.\n"
      "\n"
      "   $ zip -r -D -g %s app_name%s\n"
      "\n"
      "Printing help file...\n"
      "\n",
      program_invocation_name, APP_SUFFIX);
  print_help();
}

bool check_kernel_version(void) {
  /*
      Check the kernel to see if it is a version that should support our mount
      & namespace strategy. Per man 7 user_namespaces, OverlayFS capability
      was added in 5.11. Technically even without that unprivileged process
      might be able to mount to locations per fstab but we don't check that to
      determine default behavior; users will have to specify in that case.
  */
  struct utsname lool;
  if (uname(&lool)) {
    return FALSE;
  }
  if (strcmp(lool.sysname, "Linux")) {
    return FALSE;
  }

  // Running Linux. Is the kernel recent enough?
  char* release = xstrdup(lool.release);
  int version = atoi(strsep(&release, "."));
  if (version > LINUX_KERNEL_MAJOR_MIN) {
    return TRUE;
  } else if (version == LINUX_KERNEL_MAJOR_MIN) {
    version = atoi(strsep(&release, "."));
    if (version > LINUX_KERNEL_MINOR_MIN) {
      return TRUE;
    } else if (version == LINUX_KERNEL_MINOR_MIN) {
      version = atoi(strsep(&release, "."));
      if (version >= LINUX_KERNEL_PATCH_MIN) {
        return TRUE;
      }
    }
  }
  return FALSE;
}

void parse_options(int argc, char** argv) {
  /*
      Parse any options/arguments
      Error out with any non-option arguments.
  */

  int opt, opt_index;
  while ((opt = getopt(argc, argv, ":mno:d:w:u:h")) != -1) {
    switch (opt) {
      case 'm':
        tidy_mode = 'm';  // mount
        break;
      case 'n':
        tidy_mode = 'n';  // none
        break;
      case 'o':
        if (strcmp(optarg, "over") && strcmp(optarg, "under")) {
          fprintf(stderr, "PUISNE: Argument to -o must be in {over,under}!\n");
          exit(1);
        }
        overlay = optarg[0];
        break;
      case 'd':
        unzip_dir = optarg;
        break;
      case 'w':
        work_dir = optarg;
        break;
      case 'u':
        if (strcmp(optarg, "all") && strcmp(optarg, "new") &&
            strcmp(optarg, "existing") && strcmp(optarg, "update") &&
            strcmp(optarg, "freshen") && strcmp(optarg, "none")) {
          fprintf(stderr, "PUISNE: Argument to -u must be in {all,new,existing,"
                          "update,freshen,none}!\n");
          exit(1);
        }
        if (!strcmp(optarg, "none")) {
          unzip = '0';
        } else {
          unzip = optarg[0];
        }
        break;
      case 'h':
        print_help();  // No reason to go on, just print help & exit.
        break;
      case '?':
        fprintf(stderr, "PUISNE: Found unrecognized argument -%c!\n", optopt);
        exit(1);
        break;
      case ':':
        fprintf(stderr, "PUISNE: Missing arg for -%c!\n", optopt);
        exit(1);
        break;
    }
  }

  if (argc != optind) {
    fprintf(stderr, "PUISNE: Found non-option arguments!\n");
    exit(1);
  }

  bool kernel_version_recent = check_kernel_version();
  if (tidy_mode == 'm' && !kernel_version_recent) {
    fprintf(stderr, "PUISNE: Need Linux kernel ≥ %d.%d.%d to mount!\n",
            LINUX_KERNEL_MAJOR_MIN, LINUX_KERNEL_MINOR_MIN,
            LINUX_KERNEL_PATCH_MIN);
    exit(1);
  }
  if (!tidy_mode) {
    if (kernel_version_recent) {
      tidy_mode = 'm';
    } else {
      tidy_mode = 'n';
    }
  }

  // Set defaults that can be determined now;
  //   `unzip_dir` might depend on `name`; see `extract_files`.
  if (!work_dir) {
    work_dir = xjoinpaths(kTmpPath, "puisne.XXXXXX");
  }
  fix_path(&work_dir);

  if (unzip_dir) {
    fix_path(&unzip_dir);
  }
}

void process_args(int* argc, char*** argv) {
  /*
      Sets options based on zip/CLI args, then `argv` as args for the package.
  */

  // Might as well look at argv[0] here, too...
  invocation_dir = xdirname(program_invocation_name);

  int package_argc = 0;
  char** package_argv = NULL;
  split_args(argc, argv, &package_argc, &package_argv);
  LoadZipArgs(argc, argv);
  parse_options(*argc, *argv);

  *argc = package_argc;
  *argv = package_argv;
}

int64_t get_time_offset() {
  /*
      Gets local time offset relative to GMT.
  */

  struct tm tm;
  int64_t t;
  time(&t);
  localtime_r(&t, &tm);

  return tm.tm_gmtoff;
}

void process_package_structure() {
  /*
      Determines files in the zip object store & their metadata.
      Makes sure only expected files are present, or errors out.
  */

  struct Zipos* zip = __zipos_get();  // 🦛

  files = malloc(sizeof(char*) * (ZIP_CDIR_RECORDS(zip->cdir) + 1));
  timestamps = malloc(sizeof(int64_t) * (ZIP_CDIR_RECORDS(zip->cdir) + 1));
  modes = malloc(sizeof(int) * (ZIP_CDIR_RECORDS(zip->cdir) + 1));

  int64_t time_offset = get_time_offset();

  for (int i = 0, record_offset = ZIP_CDIR_OFFSET(zip->cdir);
       i < ZIP_CDIR_RECORDS(zip->cdir);
       i++, record_offset += ZIP_CFILE_HDRSIZE(zip->map + record_offset)) {

    char* file = xstrndup(ZIP_CFILE_NAME(zip->map + record_offset),
                          ZIP_CFILE_NAMESIZE(zip->map + record_offset));

    // Allow & ignore some PUISNE specific stuff:
    if (memcmp(file, "puisne/", 7) == 0) {
      files[i] = "";
      free(file);
      continue;
    }
    if (memcmp(file, ".args", 5) == 0) {
      files[i] = "";
      free(file);
      continue;
    }
    if (memcmp(file, ".cosmo", 6) == 0) {
      files[i] = "";
      free(file);
      continue;
    }

    // TODO: Is there a way to get timezone info without this?
    if (memcmp(file, "usr/share/zoneinfo/", 19) == 0) {
      files[i] = "";
      free(file);
      continue;
    }

    char* file_split = strsep(&file, "/");
    // file_split is whatever is in the root level,
    // file is mutated to everything below that (ie. app-relative path).

    if (file == NULL) {
      fprintf(stderr, "PUISNE: Additional file `%s` in top level!\n",
              file_split);
      exit(1);
    }

    // App folder has to end with the suffix.
    if (!_endswith(file_split, APP_SUFFIX)) {
      fprintf(stderr, "PUISNE: Problematic top-level folder `%s`!\n",
              file_split);
      exit(1);
    }

    // Disallow no-name app folder.
    if (!strcmp(xstripext(file_split), "")) {
      fprintf(stderr, "PUISNE: Invalid app folder `%s`!\n", APP_SUFFIX);
      exit(1);
    }

    // We either just learned our app's name for the first time, or need to
    // confirm it hasn't changed (ie. we have multiple .app/ folders).
    if (name) {
      if (strcmp(name, xstripext(file_split))) {  // Three's a crowd.
        fprintf(stderr, "PUISNE: Found multiple top level app folders!\n");
        exit(1);
      }
    } else {
      name = xstripext(file_split);  // Nice to meet you.
    }

    // Get metadata too:
    struct timespec modified_time;
    GetZipCfileTimestamps(zip->map + record_offset, &modified_time, NULL, NULL,
                          time_offset);
    timestamps[i] = modified_time.tv_sec;

    modes[i] = GetZipCfileMode(zip->map + record_offset);

    // Store & clean-up:
    files[i] = xstrdup(file);
    free(file_split);
  }

  if (!name) {  // If we found nothing...
    print_empty();
  }
}

void extract_file(char* zip_file, char* local_file, int mode) {
  /*
      Extract a single file¹ & remove it from our list of things to process.

      ¹or make a directory blahblahblah...
  */

  // zipOS may explicitly include directories:
  if (_endswith(zip_file, "/")) {
    makedirs(local_file, mode);
    return;
  }

  // If not, we might need to make them in advance:
  if (!isdirectory(xdirname(local_file))) {
    makedirs(xdirname(local_file), 0755);
  }

  FILE *fi, *fo;
  unsigned char buffer[BUFSIZ];
  fi = fopen(xstrcat("/zip/", name, APP_SUFFIX, '/', zip_file), "rb");
  if (!fi) {
    fprintf(stderr, "PUISNE: Zip error reading file `%s`!\n", zip_file);
    exit(1);
  }
  fo = fopen(local_file, "wb");
  if (!fo) {
    fprintf(stderr, "PUISNE: Write error extracting `%s`.\n", local_file);
    exit(1);
  }

  int count;
  while (count = fread(buffer, sizeof(buffer[0]), BUFSIZ, fi)) {
    fwrite(buffer, sizeof(buffer[0]), count, fo);
  }
  fclose(fi);
  fclose(fo);
  chmod(local_file, mode);
}

void extract_files(void) {
  /*
      Since the package in the object store checks out, extract the files to
      wherever specified by `-d`.
      This actually compares the timestamp in the zip object store & file
      on-disk, and only extracts (/overwrites) if the zipOS is newer; this is
      basically a way to cache (so you don't need to re-extract each run), or
      to maintain user/runtime edits. The latter can be propogated to the
      package itself via
      ```sh
      zip -d package.com package.app/\* \
          && zip -r package.com "package.app"
      ```
      (`zip -FS` will remove additional PUISNE/Cosmopolitan files).
  */

  // Last chance to set `unzip_dir`, if it hasn't yet...
  if (!unzip_dir) {
    if (tidy_mode == 'n') {
      unzip_dir = invocation_dir;
    } else {
      unzip_dir = xasprintf(
          "%s/.puisne/%s%s", invocation_dir,
          name,  // ...we needed this from `process_package_structure`.
          APP_SUFFIX);
    }
  }

  if (makedirs(unzip_dir, 0755)) {
    fprintf(stderr, "PUISNE: Couldn't make app folder `%s`!\n", unzip_dir);
    exit(1);
  };

  for (int i = 0; files[i]; i++) {

    if (!strlen(files[i])) {
      continue;
    }

    char* local_file = xjoinpaths(unzip_dir, files[i]);

    if (unzip == 'a') {  // Brute-force is always simple...
      extract_file(files[i], local_file, modes[i]);
      free(files[i]);
      free(local_file);
      continue;
    }

    // More selective extraction logic:
    struct stat st;
    int s = stat(local_file, &st);

    if (!s) {  // The file exists.
      switch (unzip) {
        case 'n':
          free(files[i]);
          free(local_file);
          continue;
        case 'u':
          // u & f are equivalent here...
        case 'f':
          if (st.st_ctim.tv_sec > timestamps[i]) {
            free(files[i]);
            free(local_file);
            continue;
          }
      }
    } else {  // It don't.
      switch (unzip) {
        case 'f':
          // f & e are equivalent here...
        case 'e':
          free(files[i]);
          free(local_file);
          continue;
      }
    }

    // If we made it this far, we must want this file!
    extract_file(files[i], local_file, modes[i]);
    free(files[i]);
    free(local_file);
  }

  free(files);
  free(timestamps);
  free(modes);
}

void mount_in_namespace(void) {
  /*
      If we're in Linux, use a mount namespace to overlay the extracted files
      & the package's unzip_dir.
  */

  int uid = getuid();
  int gid = getgid();
  int m;

  char* upper_dir;
  char* lower_dir;
  char* mount_data_string;

  FILE* fo;

  if (overlay == 'o') {
    upper_dir = unzip_dir;
    lower_dir = invocation_dir;
  } else {
    upper_dir = invocation_dir;
    lower_dir = unzip_dir;
  }

  work_dir = mkdtemp(work_dir);

  if (uid || gid) {  // If we aren't already root:
    // Fake it 'til you make it.
    syscall(__NR_unshare, CLONE_NEWNS | CLONE_NEWUSER);

    // Map to root in the new namespace.
    fo = fopen("/proc/self/uid_map", "w");
    fprintf(fo, "0 %d 1\n", uid);
    fclose(fo);
    fo = fopen("/proc/self/setgroups", "w");
    fprintf(fo, "deny");
    fclose(fo);
    fo = fopen("/proc/self/gid_map", "w");
    fprintf(fo, "0 %d 1", gid);
    fclose(fo);
  }

  // Handle nestedness:
  char real_lower_dir[PATH_MAX];
  char real_upper_dir[PATH_MAX];

  if (_startswith(
          realpath(lower_dir, real_lower_dir),
          realpath(upper_dir, real_upper_dir))) {  // If lower_dir is a
                                                   // subdirectory of upper_dir:

    // Make an intermediary directory within work_dir:
    char* intermediate_mnt = xjoinpaths(work_dir, "inter.mnt");
    char* intermediate_wrk = xjoinpaths(work_dir, "inter.wrk");
    if (makedirs(intermediate_mnt, 0755)) {
      fprintf(stderr, "PUISNE: Could not make intermediate directory %s!\n",
              intermediate_mnt);
    }
    if (makedirs(intermediate_wrk, 0755)) {
      fprintf(stderr, "PUISNE: Could not make intermediate directory %s!\n",
              intermediate_wrk);
    }

    // Mount an overlay there, to be used as an intermediate layer.
    mount_data_string =
        xstrcat("upperdir=", intermediate_mnt, ",lowerdir=", lower_dir,
                ",workdir=", intermediate_wrk);
    m = mount("overlay", intermediate_mnt, "overlay", 0, mount_data_string);
    free(mount_data_string);
    if (m) {
      fprintf(stderr, "PUISNE: Intermediate mount failed!\n");
      exit(1);
    }

    // Update so the "real" overlay mount uses that:
    lower_dir = intermediate_mnt;
    work_dir = xjoinpaths(work_dir, "over.wrk");
    makedirs(work_dir, 0755);
  }

  mount_data_string = xstrcat("upperdir=", upper_dir, ",lowerdir=", lower_dir,
                              ",workdir=", work_dir);
  m = mount("overlay", invocation_dir, "overlay", 0, mount_data_string);
  free(mount_data_string);
  if (m) {
    fprintf(stderr, "PUISNE: Overlay mount failed!\n");
    exit(1);
  }

  if (uid || gid) {  // If we weren't already root:
    // unshare again to drop privilege:
    syscall(__NR_unshare, CLONE_NEWUSER);

    fo = fopen("/proc/self/uid_map", "w");
    fprintf(fo, "%d 0 1\n", uid, uid);
    fclose(fo);
    fo = fopen("/proc/self/setgroups", "w");
    fprintf(fo, "deny");
    fclose(fo);
    fo = fopen("/proc/self/gid_map", "w");
    fprintf(fo, "%d 0 1\n", gid, gid);
    fclose(fo);
  }

  // One last thing, basically `cd .`; without this, if we re-mounted over the
  // CWD we would not "see" the new files.
  chdir(getcwd(0, 0));
}

void process_package_files(void) {
  /*
      Extracts files to unzip_dir, then handles any cleanup/localization
      procedures.
  */

  if (unzip != '0') {
    extract_files();
  }
  if (tidy_mode == 'm') {
    mount_in_namespace();
  }
}

void launch_package(int argc, char** argv) {
  /*
      Dooooooo it.
      Grows `cmd`, basically the full `argv` for the new process.
      In Windows, this additionally begins with `.../cmd.exe /C` to handle
      "executable" files that aren't .exe/.com/.bat/whatever; otherwise we'd
      need the `system` instead of `exec` family, which is a can of worms.
      Just support `!#` Microsoft gawd.
  */

  char* run_dir;
  if (tidy_mode == 'n') {
    run_dir = unzip_dir;
  } else {
    run_dir = invocation_dir;
  }

  char** cmd;
  int i = 0;
  if (IsWindows()) {
    // TODO: is there a better way to handle this?
    //       eg. just `call` or `exec` instead of `cmd /C`?
    //       Doesn't seem to work, even with ftype/assoc...
    //       Ugh probably not. Apparently even `|` pipelines yield additional
    //       cmd.exe invocations like this.
    cmd = malloc(sizeof(char*) * (argc + 4));
    cmd[i++] = xstrcat(kNtSystemDirectory, "cmd.exe");
    cmd[i++] = "/C";
  } else {
    cmd = malloc(sizeof(char*) * (argc + 2));
  }
  cmd[i++] = realpath(xstrcat(run_dir, '/', name), NULL);
  for (int j = 0; j < argc; j++) {
    cmd[i++] = argv[j];
  }
  cmd[i++] = '\0';

  int rc = execv(cmd[0], cmd);

  // We should never get here.
  fprintf(stderr, "PUISNE: execution error!\n");
  free(cmd);
  exit(rc);
}

int main(int argc, char** argv) {
  process_args(&argc, &argv);
  process_package_structure();
  process_package_files();
  launch_package(argc, argv);

  return 0;  // Should never get here, but it's nice to see an old friend.
}
