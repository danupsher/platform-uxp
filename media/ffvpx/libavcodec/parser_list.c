#include "config_components.h"

static const AVCodecParser * const parser_list[] = {
#if CONFIG_H264_PARSER
    &ff_h264_parser,
#endif
#if CONFIG_VP8_PARSER
    &ff_vp8_parser,
#endif
#if CONFIG_VP9_PARSER
    &ff_vp9_parser,
#endif
    NULL };
