#!/bin/bash
# Runs a command with the Khronos Vulkan validation layer enabled (one line):
#   tools/vvl/vvl.sh build/lasrecomp [args]
#   tools/vvl/vvl.sh build/xrmock/test_client 60        (the mock and its client work the same way)
# The layer is looked for in ~/.local/share/vulkan/explicit_layer.d (tools/vvl/build-vvl.sh puts it
# there; GalaxyFrame's build is the same layer and is already there on this Frame).  Messages go to
# stderr; VVL_LOG=<file> also writes them to a file.  Thread-safety checking is part of the default
# set, so queue races between the recorder thread and OpenXR show up.  VVL_SYNC=1 adds
# synchronization validation (slow).  The game's own LARECOMP_VK_VALIDATION_STOP=1 (device.c)
# makes it stop at the first validation error.  VVL_NO_WSI=1 turns off gamescope's WSI layer
# (ENABLE_GAMESCOPE_WSI=0): under gamescope it injects instance/device extensions without their
# dependencies, which shows as VUID-vkCreateInstance-ppEnabledExtensionNames-01388 and
# VUID-vkCreateDevice-ppEnabledExtensionNames-01387 noise.  Use it for offscreen runs (xrmock,
# STEREO_FLAT without a window), not for a flat window.
LAYER_DIR=${VVL_LAYER_DIR:-$HOME/.local/share/vulkan/explicit_layer.d}
[ -f "$LAYER_DIR/VkLayer_khronos_validation.json" ] || { echo "vvl.sh: no validation layer in $LAYER_DIR: run tools/vvl/build-vvl.sh" >&2; exit 1; }
export VK_LAYER_PATH=$LAYER_DIR${VK_LAYER_PATH:+:$VK_LAYER_PATH}
export VK_LOADER_LAYERS_ENABLE=VK_LAYER_KHRONOS_validation
[ -n "$VVL_LOG" ] && export VK_LAYER_LOG_FILENAME=$VVL_LOG
[ -n "$VVL_SYNC" ] && export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
[ -n "$VVL_NO_WSI" ] && export ENABLE_GAMESCOPE_WSI=0
exec "$@"
