# MAC-4: run as the JUCE fetch's PATCH_COMMAND on macOS (cmake -P, in the JUCE source folder).
# The pinned JUCE (dsp56300 fork, 7.0.10) calls CGWindowListCreateImage, which the macOS 15 SDK marks unavailable -
# a compile error even where it is not reached. With that SDK the native-window snapshot returns an empty image
# (the plugin never uses it: its editor screenshots paint the component into an Image). Idempotent.
set(f "modules/juce_gui_basics/native/juce_Windowing_mac.mm")
file(READ "${f}" src)
if(src MATCHES "MS2000R MAC-4")
    return()
endif()
set(old "        CGImageRef screenShot = CGWindowListCreateImage (CGRectNull,
                                                         kCGWindowListOptionIncludingWindow,
                                                         (CGWindowID) [nsWindow windowNumber],
                                                         kCGWindowImageBoundsIgnoreFraming);
")
set(new "       #if defined (MAC_OS_VERSION_15_0) && MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_VERSION_15_0
        // MS2000R MAC-4: obsoleted in the macOS 15 SDK - no native snapshot
        ignoreUnused (nsWindow);
        CGImageRef screenShot = nullptr;
       #else
        CGImageRef screenShot = CGWindowListCreateImage (CGRectNull,
                                                         kCGWindowListOptionIncludingWindow,
                                                         (CGWindowID) [nsWindow windowNumber],
                                                         kCGWindowImageBoundsIgnoreFraming);
       #endif
        if (screenShot == nullptr)
            return {};
")
string(FIND "${src}" "${old}" at)
if(at EQUAL -1)
    message(FATAL_ERROR "juce_patch.cmake: the CGWindowListCreateImage call was not found in ${f}")
endif()
string(REPLACE "${old}" "${new}" src "${src}")
file(WRITE "${f}" "${src}")
