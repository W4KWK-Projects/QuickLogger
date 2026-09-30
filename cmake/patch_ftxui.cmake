# Adds a frame-writer hook to FTXUI v5.0.0's ScreenInteractive, so
# QuickLogger can send only the parts of each frame that changed (see
# src/ui/frame_writer.hpp) instead of FTXUI redrawing the whole screen on
# every frame. Run by FetchContent as the FTXUI download's PATCH_COMMAND:
#
#   cmake -DFTXUI_SOURCE_DIR=<FTXUI checkout> -P cmake/patch_ftxui.cmake
#
# Each change is a plain text replacement anchored on a single line, so it
# works on checkouts with either line ending. Running it again on a patched
# checkout does nothing. If any anchor isn't found exactly once (a newer
# FTXUI), the build stops here rather than going on without the hook.

if(NOT FTXUI_SOURCE_DIR)
    message(FATAL_ERROR "patch_ftxui.cmake: FTXUI_SOURCE_DIR is not set")
endif()

set(marker "QuickLogger frame-writer patch")
set(header "${FTXUI_SOURCE_DIR}/include/ftxui/component/screen_interactive.hpp")
set(source "${FTXUI_SOURCE_DIR}/src/ftxui/component/screen_interactive.cpp")

file(READ "${header}" header_text)
file(READ "${source}" source_text)

string(FIND "${header_text}" "${marker}" header_marker)
string(FIND "${source_text}" "${marker}" source_marker)
if(NOT header_marker EQUAL -1 AND NOT source_marker EQUAL -1)
    message(STATUS "FTXUI already has the ${marker}")
    return()
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
"  if (frame_writer_) {  // ${marker}
    std::cout << frame_writer_(*this, resized || frame_writer_full_);
    frame_writer_full_ = false;
    // The writer left the cursor where it wants it; don't move it back.
    reset_cursor_position = \"\";
  } else {
    std::cout << ToString() << set_cursor_position;
  }"
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

file(WRITE "${header}" "${header_text}")
file(WRITE "${source}" "${source_text}")
message(STATUS "Applied the ${marker} to FTXUI")
