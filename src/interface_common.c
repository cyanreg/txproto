#include "interface_common.h"
#include "utils.h"
#include "../config.h"

#ifdef HAVE_WAYLAND
extern const InterfaceSystem win_wayland;
#endif

const InterfaceSystem *sp_compiled_interfaces[] = {
#ifdef HAVE_WAYLAND
    &win_wayland,
#endif
};

const int sp_compiled_interfaces_len = SP_ARRAY_ELEMS(sp_compiled_interfaces);
