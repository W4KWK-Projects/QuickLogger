# Adds a frame-writer hook to FTXUI v5.0.0's ScreenInteractive, so
# QuickLogger can send only the parts of each frame that changed (see
# src/ui/frame_writer.hpp) instead of FTXUI redrawing the whole screen on
# every frame. Run by FetchContent as the FTXUI download's PATCH_COMMAND:
#
#   cmake -DFTXUI_SOURCE_DIR=<FTXUI checkout> -P cmake/patch_ftxui.cmake
#
# Each change is a plain text replacement anchored on a single line, so it
# works on checkouts with either line ending. Running it again on a patched
# checkout does nothing; a checkout with an earlier version of the patch is
# restored with git and patched afresh. If any anchor isn't found exactly
# once (a newer FTXUI), the build stops here rather than going on without
# the hook.
#
# Version 2 (QuickLogger 1.7.8): with a writer set, a frame is flushed
# without FTXUI's trailing NUL byte (a flush marker for Emscripten only), and
# a frame with no changes writes nothing at all. Over SSH each of those
# 1-byte writes was a packet of its own: 17 or so per move through a list,
# while FTXUI's Menu animates its highlight.
#
# Version 3 (QuickLogger 2.0): the animation thread sends its task only
# after a frame asks for animation (RequestAnimationFrame), instead of every
# 15 ms whether or not anything is animating. Idle, it posted ~66 tasks a
# second that the UI loop woke up just to drop, for every session.

if(NOT FTXUI_SOURCE_DIR)
    message(FATAL_ERROR "patch_ftxui.cmake: FTXUI_SOURCE_DIR is not set")
endif()

set(marker "QuickLogger frame-writer patch")
# Bumped whenever the patch changes; CMakeLists.txt passes it too, so a
# build directory patched with an earlier version runs this again.
set(version_marker "QuickLogger frame-writer patch, version 3")
if(DEFINED PATCH_VERSION AND NOT PATCH_VERSION EQUAL 3)
    message(FATAL_ERROR "patch_ftxui.cmake: CMakeLists.txt asks for patch version "
                        "${PATCH_VERSION}, but this script is version 3")
endif()
set(header "${FTXUI_SOURCE_DIR}/include/ftxui/component/screen_interactive.hpp")
set(source "${FTXUI_SOURCE_DIR}/src/ftxui/component/screen_interactive.cpp")

file(READ "${header}" header_text)
file(READ "${source}" source_text)

string(FIND "${source_text}" "${version_marker}" source_version)
if(NOT source_version EQUAL -1)
    message(STATUS "FTXUI already has the ${version_marker}")
    return()
endif()

# An earlier version of the patch: put the two files back as FTXUI has them.
string(FIND "${header_text}" "${marker}" header_marker)
string(FIND "${source_text}" "${marker}" source_marker)
if(NOT header_marker EQUAL -1 OR NOT source_marker EQUAL -1)
    execute_process(
        COMMAND git checkout -- include/ftxui/component/screen_interactive.hpp
                src/ftxui/component/screen_interactive.cpp
        WORKING_DIRECTORY "${FTXUI_SOURCE_DIR}"
        RESULT_VARIABLE restore_result)
    if(NOT restore_result EQUAL 0)
        message(FATAL_ERROR "patch_ftxui.cmake: ${FTXUI_SOURCE_DIR} has an earlier "
                            "${marker} and couldn't be restored with git. Delete the "
                            "build directory's _deps folder and configure again.")
    endif()
    file(READ "${header}" header_text)
    file(READ "${source}" source_text)
    message(STATUS "Removed an earlier ${marker} from FTXUI")
endif()

# Replaces the one occurrence of `anchor` in the variable named `text_var`.
function(replace_once text_var anchor replacement file)
    set(text "${${text_var}}")
    string(FIND "${text}" "${anchor}" first)
    string(FIND "${text}" "${anchor}" last REVERSE)
    if(first EQUAL -1)
        message(FATAL_ERROR "patch_ftxui.cmake: \"${anchor}\" not found in ${file}. "
                            "This FTXUI version doesn't take the ${marker}; update the patch.")
    endif()
    if(NOT first EQUAL last)
        message(FATAL_ERROR "patch_ftxui.cmake: \"${anchor}\" found more than once in ${file}. "
                            "This FTXUI version doesn't take the ${marker}; update the patch.")
    endif()
    string(REPLACE "${anchor}" "${replacement}" text "${text}")
    set(${text_var} "${text}" PARENT_SCOPE)
endfunction()

# --- screen_interactive.hpp ---

replace_once(header_text
"  static ScreenInteractive* Active();"
"  static ScreenInteractive* Active();

  // ${marker}: when set, Draw() hands each rendered frame to
  // this writer and prints what it returns, instead of printing the whole
  // screen (ToString) with FTXUI's relative cursor moves. The writer owns all
  // cursor positioning, so it's for Fullscreen, where the frame's top-left is
  // the terminal's. `full` is true when the terminal's copy of the screen
  // can't be trusted: the first frame, after a resize, and after the terminal
  // was handed back (WithRestoredIO, suspend).
  using FrameWriter = std::function<std::string(const Screen& screen, bool full)>;
  void SetFrameWriter(FrameWriter writer);"
"${header}")

replace_once(header_text
"  bool frame_valid_ = false;"
"  bool frame_valid_ = false;

  // ${marker}.
  FrameWriter frame_writer_;
  bool frame_writer_full_ = true;"
"${header}")

# --- screen_interactive.cpp ---

replace_once(source_text
"void ScreenInteractive::Install() {"
"void ScreenInteractive::Install() {
  frame_writer_full_ = true;  // ${marker}"
"${source}")

replace_once(source_text
"  std::cout << ResetPosition(/*clear=*/resized);"
"  if (!frame_writer_) {  // ${marker}
    std::cout << ResetPosition(/*clear=*/resized);
  }"
"${source}")

replace_once(source_text
"  std::cout << ToString() << set_cursor_position;"
"  if (frame_writer_) {  // ${version_marker}
    const std::string frame = frame_writer_(*this, resized || frame_writer_full_);
    frame_writer_full_ = false;
    // Flushed without Flush()'s NUL, and not at all when nothing changed.
    if (!frame.empty()) {
      std::cout << frame << std::flush;
    }
    // The writer left the cursor where it wants it; don't move it back.
    reset_cursor_position = \"\";
    Clear();
    frame_valid_ = true;
    return;
  }
  std::cout << ToString() << set_cursor_position;"
"${source}")

replace_once(source_text
"void ScreenInteractive::Exit() {"
"// ${marker}.
void ScreenInteractive::SetFrameWriter(FrameWriter writer) {
  frame_writer_ = std::move(writer);
  frame_writer_full_ = true;
}

void ScreenInteractive::Exit() {"
"${source}")

# Version 3: the animation thread waits until a frame asks for animation.
replace_once(source_text
"#include <thread>    // for thread, sleep_for"
"#include <thread>    // for thread, sleep_for
#include <condition_variable>  // ${version_marker}
#include <mutex>"
"${source}")

replace_once(source_text
"    out->Send(AnimationTask());"
"    {  // ${version_marker}: only once a frame asks for animation.
      std::unique_lock<std::mutex> lock(g_animation_mutex);
      if (!g_animation_wanted) {
        g_animation_wake.wait_for(lock, std::chrono::milliseconds(200));
      }
      if (!g_animation_wanted) {
        continue;
      }
      g_animation_wanted = false;
    }
    out->Send(AnimationTask());"
"${source}")

replace_once(source_text
"void AnimationListener(std::atomic<bool>* quit, Sender<Task> out) {"
"// Set by RequestAnimationFrame; ${version_marker}.
std::mutex g_animation_mutex;
std::condition_variable g_animation_wake;
bool g_animation_wanted = false;

void AnimationListener(std::atomic<bool>* quit, Sender<Task> out) {"
"${source}")

replace_once(source_text
"  animation_requested_ = true;"
"  animation_requested_ = true;
  {  // ${version_marker}
    std::lock_guard<std::mutex> lock(g_animation_mutex);
    g_animation_wanted = true;
  }
  g_animation_wake.notify_one();"
"${source}")

file(WRITE "${header}" "${header_text}")
file(WRITE "${source}" "${source_text}")
message(STATUS "Applied the ${marker} to FTXUI")
