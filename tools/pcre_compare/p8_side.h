/* p8_side.h -- PCRE 8.45, behind names that can sit beside mc_pcre.h. */

#ifdef __cplusplus
extern "C" {
#endif

void * p8_compile (const char * pattern, int options, const char ** error, int * erroffset);
void * p8_study (void * code, int jit);
int p8_jit_active (void * extra);
int p8_exec (void * code, void * extra, const char * subject, int length, int start, int options,
             int * ovector, int ovecsize);
int p8_capture_count (void * code);
int p8_name_table (void * code, const unsigned char ** table, int * entry_size);
void p8_free (void * code, void * extra);
const char * p8_version (void);
/* Runs with a callout that returns callout_result and logs what it is told as
   ints: number, capture_top, capture_last, start_match, current_position, then
   2 * capture_top offsets.  Returns the match's result. */
int p8_exec_logging (void * code, void * extra, const char * subject, int length, int options,
                     int * ovector, int ovecsize, int callout_result, int * log, int * log_used, int log_size);

#ifdef __cplusplus
}
#endif
