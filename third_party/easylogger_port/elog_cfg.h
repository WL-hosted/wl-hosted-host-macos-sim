#ifndef _ELOG_CFG_H_
#define _ELOG_CFG_H_

#define ELOG_OUTPUT_ENABLE
#define ELOG_OUTPUT_LVL                          ELOG_LVL_VERBOSE
#define ELOG_ASSERT_ENABLE
#define ELOG_LINE_BUF_SIZE                       1024
#define ELOG_LINE_NUM_MAX_LEN                    5
#define ELOG_FILTER_TAG_MAX_LEN                  30
#define ELOG_FILTER_KW_MAX_LEN                   16
#define ELOG_FILTER_TAG_LVL_MAX_NUM              5
#define ELOG_NEWLINE_SIGN                        "\n"

#define ELOG_COLOR_ENABLE
#define ELOG_COLOR_ASSERT                        (F_MAGENTA B_NULL S_NORMAL)
#define ELOG_COLOR_ERROR                         (F_RED     B_NULL S_NORMAL)
#define ELOG_COLOR_WARN                          (F_YELLOW  B_NULL S_NORMAL)
#define ELOG_COLOR_INFO                          (F_CYAN    B_NULL S_NORMAL)
#define ELOG_COLOR_DEBUG                         (F_GREEN   B_NULL S_NORMAL)
#define ELOG_COLOR_VERBOSE                       (F_BLUE    B_NULL S_NORMAL)

#define ELOG_FMT_USING_FUNC
#define ELOG_FMT_USING_DIR
#define ELOG_FMT_USING_LINE

/* Synchronous output only for the simulator. */
/* #define ELOG_ASYNC_OUTPUT_ENABLE */
/* #define ELOG_BUF_OUTPUT_ENABLE */

#endif /* _ELOG_CFG_H_ */
