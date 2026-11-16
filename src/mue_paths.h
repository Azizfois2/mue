/* mue_paths.h - the install tree.
 *
 * emu8086 is not a lone .exe: the editor reads its start-up templates from
 * inc\, the examples menu reads examples\, every build lands in MyBuild\ as
 * five files whose names end in an underscore, the editor colours live in
 * Options\, and the virtual drive is vdrive\<letter>\.  re/FILESYSTEM.md
 * records that tree; this module is the single place that knows where it
 * lives, so no other source file needs a literal path.
 */
#ifndef MUE_PATHS_H
#define MUE_PATHS_H

/* Longest install path mue builds by hand ("C:\mue\MyBuild\name.com_.list"). */
#ifndef MUE_MAXP
#  define MUE_MAXP 700
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Absolute install root, e.g. "C:\mue".  Resolved once, in this order:
 *   1. $MUE_HOME, when set (created if missing),
 *   2. the folder above this executable, when it already holds inc\ or
 *      examples\ (a shipped layout),
 *   3. C:\mue,
 *   4. C:\emu8086 - the reference install, when C:\mue cannot be created.
 */
const char *mue_home(void);

/* home\<rel>, written into buf.  Returns buf. */
const char *mue_home_path(char *buf, int bufsz, const char *rel);

/* Subdirectories of the install root. */
const char *mue_dir_examples(void);
const char *mue_dir_inc(void);
const char *mue_dir_build(void);
const char *mue_dir_source(void);
const char *mue_dir_options(void);
const char *mue_dir_vdrive(void);
const char *mue_dir_docs(void);

/* Create every missing directory along `path` ("mkdir -p").  Returns 1 when
 * the final directory exists afterwards. */
int mue_mkdirs(const char *path);

/* Create the documented tree and seed the files the UI reads, once per run.
 * Existing files are never overwritten: the templates and the Options records
 * are user-editable, exactly as in emu8086. */
void mue_tree_ensure(void);

/* Read a whole file into a malloc'd, NUL-terminated buffer (NULL if absent). */
char *mue_read_file(const char *path);

/* Write `len` bytes to `path`, creating the parent directories. */
int mue_write_file(const char *path, const void *data, int len);

#ifdef __cplusplus
}
#endif
#endif
