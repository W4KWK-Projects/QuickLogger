#include "pages.hpp"

#include <cstddef>
#include <utility>

#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>

#include "chrome.hpp"
#include "handlers.hpp"

namespace ql
{

    static ftxui::Element ErrorLine(const std::string& message)
    {
        return message.empty() ? ftxui::text("")
                               : ftxui::text(message) | ftxui::color(ftxui::Color::Red);
    }

    static ftxui::Element FieldLabel(const std::string& label)
    {
        return ftxui::text(label) | ftxui::color(ftxui::Color::YellowLight);
    }

    // Same as FTXUI's own DefaultOptionTransform (menu.cpp) -- the "> "/"  "
    // prefix, inverted colors for the keyboard-focused entry -- minus the
    // `| bold` it applies to the selected entry. Confirmed via a user
    // screenshot: at least one real terminal (ZOC) renders bold glyphs
    // measurably wider than regular ones, so bolding a long row in a
    // column-aligned list (net-history instance list, check-in list, etc.)
    // pushed every column after the bold run out from under its header --
    // visible only on whichever row happened to be selected. The "> "
    // prefix already marks the selection; bold added no information here,
    // just an alignment bug in that terminal. Pass this as
    // `MenuOption.entries_option.transform` for any Menu whose rows must
    // stay lined up under a header.
    static ftxui::Element AlignedMenuEntryTransform(const ftxui::EntryState& state)
    {
        std::string label = (state.active ? "> " : "  ") + state.label;
        ftxui::Element element = ftxui::text(std::move(label));
        if (state.focused)
        {
            element = element | ftxui::inverted;
        }
        return element;
    }

    // The Input components for every editable Station field. Shared by the
    // New Station modal, the Edit Check-in modal, and the edit-net page's
    // saved-station form, since all three collect the same identity fields.
    // `callsign` is built separately by each caller (New Station wires
    // on_enter/on_change to it for autocomplete; Edit Check-in doesn't show it
    // at all, since callsign isn't editable there).
    struct StationFieldInputs
    {
        ftxui::Component callsign;
        ftxui::Component name;
        ftxui::Component member_id;
        ftxui::Component street_address;
        ftxui::Component city;
        ftxui::Component county;
        ftxui::Component state;
        ftxui::Component zip;
        ftxui::Component grid_square;
    };

    // InputOption shared by every single-line text field in the app: plain
    // text entry, so Enter doesn't insert a newline (FTXUI's Input default is
    // multiline, which otherwise corrupts the field and swallows the Enter
    // keypress that on_enter handlers rely on).
    static ftxui::InputOption SingleLineInputOption()
    {
        ftxui::InputOption option;
        option.multiline = false;
        return option;
    }

    // Builds every StationFieldInputs field except `callsign` (the caller
    // supplies that, since it needs different wiring per form) bound to
    // `station`'s fields.
    static StationFieldInputs BuildStationFieldInputs(Station* station,
                                                      ftxui::Component callsign_input)
    {
        StationFieldInputs inputs;
        inputs.callsign = std::move(callsign_input);
        inputs.name = ftxui::Input(&station->name, "Name", SingleLineInputOption());
        inputs.member_id = ftxui::Input(&station->member_id, "Member ID", SingleLineInputOption());
        inputs.street_address =
            ftxui::Input(&station->street_address, "Street Address", SingleLineInputOption());
        inputs.city = ftxui::Input(&station->city, "City", SingleLineInputOption());
        inputs.county = ftxui::Input(&station->county, "County", SingleLineInputOption());
        inputs.state = ftxui::Input(&station->state, "State", SingleLineInputOption());
        inputs.zip = ftxui::Input(&station->zip, "Zip", SingleLineInputOption());
        inputs.grid_square =
            ftxui::Input(&station->grid_square, "Grid Square", SingleLineInputOption());
        return inputs;
    }

    // All of `inputs`' components, in Tab order, for adding to a
    // Container::Vertical. Omits `callsign` when it's null (Edit Check-in has
    // no callsign Input at all).
    static ftxui::Components StationFieldComponents(const StationFieldInputs& inputs)
    {
        ftxui::Components components;
        if (inputs.callsign)
        {
            components.push_back(inputs.callsign);
        }
        components.push_back(inputs.name);
        components.push_back(inputs.member_id);
        components.push_back(inputs.street_address);
        components.push_back(inputs.city);
        components.push_back(inputs.county);
        components.push_back(inputs.state);
        components.push_back(inputs.zip);
        components.push_back(inputs.grid_square);
        return components;
    }

    // Renders `inputs` as labeled rows, in the same order as
    // StationFieldComponents, for embedding in a page/modal's layout.
    static ftxui::Elements StationFieldRows(const StationFieldInputs& inputs)
    {
        ftxui::Elements rows;
        if (inputs.callsign)
        {
            rows.push_back(ftxui::hbox({FieldLabel("Callsign:      "), inputs.callsign->Render()}));
        }
        rows.push_back(ftxui::hbox({FieldLabel("Name:          "), inputs.name->Render()}));
        rows.push_back(ftxui::hbox({FieldLabel("Member ID:     "), inputs.member_id->Render()}));
        rows.push_back(
            ftxui::hbox({FieldLabel("Street Addr:   "), inputs.street_address->Render()}));
        rows.push_back(ftxui::hbox({FieldLabel("City:          "), inputs.city->Render()}));
        rows.push_back(ftxui::hbox({FieldLabel("County:        "), inputs.county->Render()}));
        rows.push_back(ftxui::hbox({FieldLabel("State:         "), inputs.state->Render()}));
        rows.push_back(ftxui::hbox({FieldLabel("Zip:           "), inputs.zip->Render()}));
        rows.push_back(ftxui::hbox({FieldLabel("Grid Square:   "), inputs.grid_square->Render()}));
        return rows;
    }

    // ---- Net list page ---------------------------------------------------

    class NetListRenderer
    {
    public:
        NetListRenderer(AppState* state, ftxui::Component net_menu)
            : state_(state), net_menu_(std::move(net_menu))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Element net_list_elem =
                state_->nets.empty()
                    ? ftxui::text("No recurring nets yet. Press F2 to create one.") | ftxui::dim
                    : net_menu_->Render();

            ftxui::Element callsign_hint =
                state_->settings.callsign.empty()
                    ? ftxui::text("No callsign set -- see Settings (F4)") |
                          ftxui::color(ftxui::Color::YellowLight)
                    : ftxui::text("Operating as " + state_->settings.callsign) |
                          ftxui::color(ftxui::Color::Cyan);

            ftxui::Element content = ftxui::vbox({
                callsign_hint,
                ftxui::separator(),
                ftxui::text("Recurring Nets") | ftxui::bold | ftxui::color(ftxui::Color::Cyan),
                (net_list_elem | ftxui::border) | ftxui::flex,
                ErrorLine(state_->form_error),
            });

            return PageChrome("Recurring Nets", content,
                              {
                                  {"F2", "New"},
                                  {"F3/Enter", "Start"},
                                  {"F4", "Settings"},
                                  {"F5", "Ad Hoc"},
                                  {"F6", "History"},
                                  {"F7", "Edit"},
                                  {"F10", "Quit"},
                              });
        }

    private:
        AppState* state_;
        ftxui::Component net_menu_;
    };

    ftxui::Component BuildNetListPage(AppState* state)
    {
        ftxui::MenuOption net_menu_option;
        net_menu_option.on_enter = StartSelectedNetHandler(state);
        ftxui::Component net_menu =
            ftxui::Menu(&state->net_names, &state->selected_net_index, net_menu_option);

        ftxui::Component root = ftxui::Container::Vertical({net_menu});
        return ftxui::Renderer(root, NetListRenderer(state, net_menu));
    }

    // ---- Create-net page ---------------------------------------------------

    class CreateNetRenderer
    {
    public:
        CreateNetRenderer(AppState* state, ftxui::Component input_name, ftxui::Component input_mode,
                          ftxui::Component input_frequency, ftxui::Component input_location,
                          ftxui::Component input_recurrence)
            : state_(state),
              input_name_(std::move(input_name)),
              input_mode_(std::move(input_mode)),
              input_frequency_(std::move(input_frequency)),
              input_location_(std::move(input_location)),
              input_recurrence_(std::move(input_recurrence))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Element content = ftxui::vbox({
                ftxui::hbox({FieldLabel("Name:       "), input_name_->Render()}),
                ftxui::hbox({FieldLabel("Mode:       "), input_mode_->Render()}),
                ftxui::hbox({FieldLabel("Frequency:  "), input_frequency_->Render()}),
                ftxui::hbox({FieldLabel("Location:   "), input_location_->Render()}),
                ftxui::hbox({FieldLabel("Recurrence: "), input_recurrence_->Render()}),
                ErrorLine(state_->form_error),
            });

            return PageChrome("New Recurring Net", content, {{"F2", "Save"}, {"Esc", "Cancel"}});
        }

    private:
        AppState* state_;
        ftxui::Component input_name_;
        ftxui::Component input_mode_;
        ftxui::Component input_frequency_;
        ftxui::Component input_location_;
        ftxui::Component input_recurrence_;
    };

    ftxui::Component BuildCreateNetPage(AppState* state)
    {
        ftxui::Component input_name =
            ftxui::Input(&state->new_net_name, "e.g. Weekly Skywarn Net", SingleLineInputOption());
        ftxui::Component input_mode =
            ftxui::Input(&state->new_net_mode, "e.g. FM, SSB, Digital", SingleLineInputOption());
        ftxui::Component input_frequency =
            ftxui::Input(&state->new_net_frequency, "e.g. 146.940", SingleLineInputOption());
        ftxui::Component input_location =
            ftxui::Input(&state->new_net_location, "City, county, or ZIP", SingleLineInputOption());
        ftxui::Component input_recurrence = ftxui::Input(
            &state->new_net_recurrence, "e.g. Tuesdays 8pm ET", SingleLineInputOption());

        ftxui::Component root = ftxui::Container::Vertical({
            input_name,
            input_mode,
            input_frequency,
            input_location,
            input_recurrence,
        });

        state->new_net_name_input = input_name;

        return ftxui::Renderer(
            root, CreateNetRenderer(state, input_name, input_mode, input_frequency, input_location,
                                    input_recurrence));
    }

    // ---- Select-role page ---------------------------------------------------

    class SelectRoleRenderer
    {
    public:
        SelectRoleRenderer(AppState* state, ftxui::Component role_radiobox)
            : state_(state), role_radiobox_(std::move(role_radiobox))
        {
        }

        ftxui::Element operator()() const
        {
            std::string net_name;
            if (state_->selected_net_index < static_cast<int>(state_->nets.size()))
            {
                net_name = state_->nets[state_->selected_net_index].name;
            }

            ftxui::Element content = ftxui::vbox({
                ftxui::text("Starting: " + net_name) | ftxui::bold |
                    ftxui::color(ftxui::Color::Green),
                ftxui::separator(),
                ftxui::text("Select your role for this net:"),
                role_radiobox_->Render(),
            });

            return PageChrome("Select Your Role", content,
                              {{"F2/Enter", "Continue"}, {"Esc", "Back"}});
        }

    private:
        AppState* state_;
        ftxui::Component role_radiobox_;
    };

    ftxui::Component BuildSelectRolePage(AppState* state)
    {
        // `focused_entry` is bound to the same variable as `selected` so
        // arrow-key movement commits immediately (Radiobox otherwise only
        // writes `hovered_` into `*selected` on an explicit Space/Enter --
        // see RadioboxBase::OnEvent) -- necessary so SelectRoleKeyHandler can
        // treat a bare Enter as "continue with whatever's currently
        // highlighted" (see below) without an extra hover-commit step first.
        ftxui::RadioboxOption role_option;
        role_option.entries = &state->role_labels;
        role_option.selected = &state->selected_role_index;
        role_option.focused_entry = &state->selected_role_index;
        ftxui::Component role_radiobox = ftxui::Radiobox(role_option);

        ftxui::Component root = ftxui::Container::Vertical({role_radiobox});
        return ftxui::Renderer(root, SelectRoleRenderer(state, role_radiobox));
    }

    // ---- Enter-callsign page ---------------------------------------------------

    class EnterCallsignRenderer
    {
    public:
        EnterCallsignRenderer(AppState* state, ftxui::Component input_callsign)
            : state_(state), input_callsign_(std::move(input_callsign))
        {
        }

        ftxui::Element operator()() const
        {
            std::string role_label = state_->role_labels[state_->selected_role_index];

            ftxui::Element content = ftxui::vbox({
                ftxui::text("Role: " + role_label) | ftxui::bold |
                    ftxui::color(ftxui::Color::Magenta),
                ftxui::separator(),
                ftxui::hbox({FieldLabel("Callsign: "), input_callsign_->Render()}),
                ErrorLine(state_->form_error),
            });

            return PageChrome("Enter Callsign", content,
                              {{"F2/Enter", "Start Net"}, {"Esc", "Back"}});
        }

    private:
        AppState* state_;
        ftxui::Component input_callsign_;
    };

    ftxui::Component BuildEnterCallsignPage(AppState* state)
    {
        ftxui::InputOption input_option;
        input_option.multiline = false;
        input_option.on_enter = OperatorCallsignSubmitHandler(state);
        input_option.on_change = UppercaseFieldHandler(&state->operator_callsign);
        ftxui::Component input_callsign = ftxui::Input(&state->operator_callsign, "", input_option);

        ftxui::Component root = ftxui::Container::Vertical({input_callsign});
        return ftxui::Renderer(root, EnterCallsignRenderer(state, input_callsign));
    }

    // ---- Active-net page ---------------------------------------------------

    class ActiveNetRenderer
    {
    public:
        ActiveNetRenderer(AppState* state, ftxui::Component check_in_menu)
            : state_(state), check_in_menu_(std::move(check_in_menu))
        {
        }

        ftxui::Element operator()() const
        {
            std::string role_label = state_->role_labels[state_->selected_role_index];

            ftxui::Element check_in_list =
                state_->active_display_rows.empty()
                    ? ftxui::text("No check-ins yet.") | ftxui::dim
                    : check_in_menu_->Render() | ftxui::frame | ftxui::vscroll_indicator;

            ftxui::Element info_line = ftxui::hbox({
                ftxui::text("Date: ") | ftxui::dim,
                ftxui::text(state_->active_instance.instance_date) |
                    ftxui::color(ftxui::Color::Cyan),
                ftxui::text("   Role: ") | ftxui::dim,
                ftxui::text(role_label) | ftxui::color(ftxui::Color::Magenta),
                ftxui::text("   Callsign: ") | ftxui::dim,
                ftxui::text(state_->operator_callsign) | ftxui::color(ftxui::Color::YellowLight),
            });

            ftxui::Element content = ftxui::vbox({
                info_line,
                ftxui::separator(),
                (ftxui::vbox({
                     ftxui::text(FormatCheckInHeaderRow(/*above_menu=*/true)) | ftxui::bold |
                         ftxui::color(ftxui::Color::Cyan),
                     check_in_list,
                 }) |
                 ftxui::border) |
                    ftxui::flex,
                state_->show_new_station_modal || state_->show_edit_checkin_modal
                    ? ftxui::text("")
                    : ftxui::text("Enter or F3 to edit a highlighted check-in.") | ftxui::dim,
                ErrorLine(state_->form_error),
            });

            std::string page_title =
                state_->active_net_name.empty() ? "Active Net" : state_->active_net_name;
            // F3/F4/F5 only do anything when no modal is open (see
            // ActiveNetKeyHandler) -- showing them while one is up would be a
            // hint for a key that currently does nothing, so they're hidden
            // then, and F2 (context-sensitive) is relabeled for whichever
            // modal has focus instead.
            std::vector<KeyHint> hints;
            if (state_->show_edit_checkin_modal)
            {
                hints = {{"F2", "Save"}, {"Esc", "Cancel"}};
            }
            else if (state_->show_new_station_modal)
            {
                hints = {{"F2", "Log & Continue"}, {"Esc", "Log & Close"}};
            }
            else
            {
                hints = {
                    {"F2", "New Station"},
                    {"F3", "Edit Check-In"},
                    {"F4", "Close/Save Net"},
                    {"F5", "Delete Check-In"},
                };
            }
            return PageChrome(page_title, content, hints);
        }

    private:
        AppState* state_;
        ftxui::Component check_in_menu_;
    };

    // The New Station modal, shown on top of the active-net page.
    class NewStationModalRenderer
    {
    public:
        NewStationModalRenderer(AppState* state, StationFieldInputs inputs,
                                ftxui::Component suggestion_menu,
                                ftxui::Component input_signal_report,
                                ftxui::Component input_remarks, ftxui::Component input_comment,
                                ftxui::Component role_choice_menu)
            : state_(state),
              inputs_(std::move(inputs)),
              suggestion_menu_(std::move(suggestion_menu)),
              input_signal_report_(std::move(input_signal_report)),
              input_remarks_(std::move(input_remarks)),
              input_comment_(std::move(input_comment)),
              role_choice_menu_(std::move(role_choice_menu))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Element suggestions =
                state_->modal_callsign_suggestions.empty()
                    ? ftxui::text("")
                    : ftxui::vbox({
                          ftxui::text("Matches (Enter picks highlighted; Tab to browse):") |
                              ftxui::dim,
                          (ftxui::vbox({
                               ftxui::text(FormatCallsignSuggestionHeaderRow()) | ftxui::dim,
                               suggestion_menu_->Render(),
                           }) |
                           ftxui::border),
                      });

            ftxui::Elements rows;
            rows.push_back(ftxui::text("Log Station Check-In") | ftxui::bold |
                           ftxui::color(ftxui::Color::Cyan));
            rows.push_back(ftxui::separator());
            ftxui::Elements field_rows = StationFieldRows(inputs_);
            rows.push_back(field_rows[0]);  // Callsign
            rows.push_back(suggestions);
            for (std::size_t i = 1; i < field_rows.size(); ++i)
            {
                rows.push_back(field_rows[i]);
            }
            rows.push_back(
                ftxui::hbox({FieldLabel("Signal Report: "), input_signal_report_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Remarks:       "), input_remarks_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Comment:       "), input_comment_->Render()}));
            rows.push_back(FieldLabel("Additional Role (optional):"));
            rows.push_back(role_choice_menu_->Render());
            rows.push_back(ftxui::separator());
            rows.push_back(KeyHintRow({{"F2", "Log & Continue"}, {"Esc", "Log & Close"}}));
            rows.push_back(ErrorLine(state_->form_error));

            return ftxui::vbox(rows) | ftxui::border | ftxui::color(ftxui::Color::Cyan);
        }

    private:
        AppState* state_;
        StationFieldInputs inputs_;
        ftxui::Component suggestion_menu_;
        ftxui::Component input_signal_report_;
        ftxui::Component input_remarks_;
        ftxui::Component input_comment_;
        ftxui::Component role_choice_menu_;
    };

    // The Edit Check-in modal, shown on top of the active-net page. Callsign
    // isn't editable (see AppState::edit_checkin_original), so it's a plain
    // label rather than an Input (StationFieldInputs.callsign is left null).
    class EditCheckInModalRenderer
    {
    public:
        EditCheckInModalRenderer(AppState* state, StationFieldInputs inputs,
                                 ftxui::Component input_signal_report,
                                 ftxui::Component input_remarks, ftxui::Component input_comment,
                                 ftxui::Component role_choice_menu)
            : state_(state),
              inputs_(std::move(inputs)),
              input_signal_report_(std::move(input_signal_report)),
              input_remarks_(std::move(input_remarks)),
              input_comment_(std::move(input_comment)),
              role_choice_menu_(std::move(role_choice_menu))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Elements rows;
            rows.push_back(ftxui::text("Edit Check-In") | ftxui::bold |
                           ftxui::color(ftxui::Color::Magenta));
            rows.push_back(ftxui::separator());
            rows.push_back(ftxui::hbox({FieldLabel("Callsign:      "),
                                        ftxui::text(state_->edit_checkin_original.callsign)}));
            for (const ftxui::Element& row : StationFieldRows(inputs_))
            {
                rows.push_back(row);
            }
            rows.push_back(
                ftxui::hbox({FieldLabel("Signal Report: "), input_signal_report_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Remarks:       "), input_remarks_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Comment:       "), input_comment_->Render()}));
            rows.push_back(FieldLabel("Additional Role (optional):"));
            rows.push_back(role_choice_menu_->Render());
            rows.push_back(ftxui::separator());
            rows.push_back(KeyHintRow({{"F2", "Save"}, {"Esc", "Cancel"}}));

            return ftxui::vbox(rows) | ftxui::border | ftxui::color(ftxui::Color::Magenta);
        }

    private:
        AppState* state_;
        StationFieldInputs inputs_;
        ftxui::Component input_signal_report_;
        ftxui::Component input_remarks_;
        ftxui::Component input_comment_;
        ftxui::Component role_choice_menu_;
    };

    ftxui::Component BuildActiveNetPage(AppState* state)
    {
        ftxui::MenuOption check_in_menu_option;
        check_in_menu_option.on_enter = EditSelectedCheckInHandler(state);
        check_in_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component check_in_menu = ftxui::Menu(
            &state->active_display_rows, &state->selected_check_in_index, check_in_menu_option);

        ftxui::Component main_root = ftxui::Container::Vertical({check_in_menu});
        ftxui::Component main_view =
            ftxui::Renderer(main_root, ActiveNetRenderer(state, check_in_menu));

        ftxui::InputOption callsign_option;
        callsign_option.multiline = false;
        callsign_option.on_enter = CallsignLookupHandler(state);
        callsign_option.on_change = CallsignSuggestHandler(state);
        ftxui::Component input_callsign =
            ftxui::Input(&state->modal_station.callsign, "Callsign", callsign_option);
        StationFieldInputs modal_inputs =
            BuildStationFieldInputs(&state->modal_station, input_callsign);

        ftxui::MenuOption suggestion_menu_option;
        suggestion_menu_option.on_enter = SelectCallsignSuggestionHandler(state);
        suggestion_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component suggestion_menu =
            ftxui::Menu(&state->modal_callsign_suggestion_labels, &state->selected_suggestion_index,
                        suggestion_menu_option);

        ftxui::Component input_signal_report =
            ftxui::Input(&state->modal_signal_report, "Signal Report", SingleLineInputOption());
        ftxui::Component input_remarks =
            ftxui::Input(&state->modal_remarks, "Remarks", SingleLineInputOption());
        ftxui::Component input_comment =
            ftxui::Input(&state->modal_comment, "Comment", SingleLineInputOption());
        // A Menu (not Radiobox) so arrow keys change the choice immediately --
        // no separate "confirm with Space/Enter" step, and no internal hover
        // cursor left dangling from a previous check-in. A Radiobox retains
        // its own hovered-entry state across the component's whole lifetime
        // (it's built once, like every other component in this app), so if
        // this modal is reopened for a second check-in after resetting
        // modal_role_choice_index to 0, the *hover* wouldn't reset with it --
        // the next arrow-down would move relative to wherever it was left
        // after the previous check-in, silently landing on the wrong role.
        ftxui::Component role_choice_menu =
            ftxui::Menu(&state->modal_role_choice_labels, &state->modal_role_choice_index);

        state->modal_callsign_input = input_callsign;

        ftxui::Components modal_components = StationFieldComponents(modal_inputs);
        modal_components.push_back(suggestion_menu);
        modal_components.push_back(input_signal_report);
        modal_components.push_back(input_remarks);
        modal_components.push_back(input_comment);
        modal_components.push_back(role_choice_menu);
        ftxui::Component modal_root = ftxui::Container::Vertical(modal_components);
        ftxui::Component modal_view = ftxui::Renderer(
            modal_root,
            NewStationModalRenderer(state, modal_inputs, suggestion_menu, input_signal_report,
                                    input_remarks, input_comment, role_choice_menu));

        StationFieldInputs edit_checkin_inputs =
            BuildStationFieldInputs(&state->edit_checkin_station, ftxui::Component());
        ftxui::Component edit_input_signal_report = ftxui::Input(
            &state->edit_checkin_signal_report, "Signal Report", SingleLineInputOption());
        ftxui::Component edit_input_remarks =
            ftxui::Input(&state->edit_checkin_remarks, "Remarks", SingleLineInputOption());
        ftxui::Component edit_input_comment =
            ftxui::Input(&state->edit_checkin_comment, "Comment", SingleLineInputOption());
        ftxui::Component edit_role_choice_menu = ftxui::Menu(
            &state->edit_checkin_role_choice_labels, &state->edit_checkin_role_choice_index);

        ftxui::Components edit_modal_components = StationFieldComponents(edit_checkin_inputs);
        edit_modal_components.push_back(edit_input_signal_report);
        edit_modal_components.push_back(edit_input_remarks);
        edit_modal_components.push_back(edit_input_comment);
        edit_modal_components.push_back(edit_role_choice_menu);
        ftxui::Component edit_modal_root = ftxui::Container::Vertical(edit_modal_components);
        ftxui::Component edit_modal_view = ftxui::Renderer(
            edit_modal_root, EditCheckInModalRenderer(state, edit_checkin_inputs,
                                                      edit_input_signal_report, edit_input_remarks,
                                                      edit_input_comment, edit_role_choice_menu));

        ftxui::Component with_new_station_modal =
            ftxui::Modal(main_view, modal_view, &state->show_new_station_modal);
        return ftxui::Modal(with_new_station_modal, edit_modal_view,
                            &state->show_edit_checkin_modal);
    }

    // ---- Settings page ---------------------------------------------------

    class SettingsRenderer
    {
    public:
        SettingsRenderer(AppState* state, ftxui::Component input_callsign,
                         ftxui::Component input_location, ftxui::Component input_qrz_username,
                         ftxui::Component input_qrz_password)
            : state_(state),
              input_callsign_(std::move(input_callsign)),
              input_location_(std::move(input_location)),
              input_qrz_username_(std::move(input_qrz_username)),
              input_qrz_password_(std::move(input_qrz_password))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Element content = ftxui::vbox({
                ftxui::hbox({FieldLabel("My Callsign*: "), input_callsign_->Render()}),
                ftxui::hbox({FieldLabel("My ZIP Code*: "), input_location_->Render()}),
                ftxui::hbox({FieldLabel("QRZ Username: "), input_qrz_username_->Render()}),
                ftxui::hbox({FieldLabel("QRZ Password: "), input_qrz_password_->Render()}),
                ftxui::text("* Required") | ftxui::dim,
                ftxui::separator(),
                ftxui::text("My ZIP Code is a plain 5-digit US ZIP code (digits only), used to "
                            "find nearby ULS stations when saving a station to a net.") |
                    ftxui::dim,
                ftxui::text(
                    "QRZ credentials are optional; used later for looking up station info.") |
                    ftxui::dim,
                ftxui::text("Never included when the database is exported.") | ftxui::dim,
                ftxui::separator(),
                ftxui::text(DescribeUlsImportStatus(state_)) | ftxui::color(ftxui::Color::Cyan),
                ErrorLine(state_->form_error),
            });

            return PageChrome("Settings", content,
                              {{"F2", "Save"}, {"F3", "Import ULS"}, {"Esc", "Cancel"}});
        }

    private:
        AppState* state_;
        ftxui::Component input_callsign_;
        ftxui::Component input_location_;
        ftxui::Component input_qrz_username_;
        ftxui::Component input_qrz_password_;
    };

    ftxui::Component BuildSettingsPage(AppState* state)
    {
        ftxui::InputOption callsign_option = SingleLineInputOption();
        callsign_option.on_change = UppercaseFieldHandler(&state->settings_form.callsign);
        ftxui::Component input_callsign =
            ftxui::Input(&state->settings_form.callsign, "Your callsign", callsign_option);
        ftxui::InputOption location_option = SingleLineInputOption();
        location_option.on_change = ZipCodeFieldHandler(&state->settings_form.location);
        ftxui::Component input_location =
            ftxui::Input(&state->settings_form.location, "5-digit ZIP", location_option);
        ftxui::Component input_qrz_username =
            ftxui::Input(&state->settings_form.qrz_username, "Optional", SingleLineInputOption());

        ftxui::InputOption password_option;
        password_option.multiline = false;
        password_option.password = true;
        ftxui::Component input_qrz_password =
            ftxui::Input(&state->settings_form.qrz_password, "Optional", password_option);

        ftxui::Component root = ftxui::Container::Vertical({
            input_callsign,
            input_location,
            input_qrz_username,
            input_qrz_password,
        });

        return ftxui::Renderer(root, SettingsRenderer(state, input_callsign, input_location,
                                                      input_qrz_username, input_qrz_password));
    }

    // ---- Ad hoc net page ---------------------------------------------------

    class AdHocNetRenderer
    {
    public:
        AdHocNetRenderer(AppState* state, ftxui::Component input_name, ftxui::Component input_mode,
                         ftxui::Component input_frequency, ftxui::Component input_location)
            : state_(state),
              input_name_(std::move(input_name)),
              input_mode_(std::move(input_mode)),
              input_frequency_(std::move(input_frequency)),
              input_location_(std::move(input_location))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Element content = ftxui::vbox({
                ftxui::text("Logs a one-off net without saving it as a recurring net.") |
                    ftxui::dim,
                ftxui::separator(),
                ftxui::hbox({FieldLabel("Name:      "), input_name_->Render()}),
                ftxui::hbox({FieldLabel("Mode:      "), input_mode_->Render()}),
                ftxui::hbox({FieldLabel("Frequency: "), input_frequency_->Render()}),
                ftxui::hbox({FieldLabel("Location:  "), input_location_->Render()}),
                ErrorLine(state_->form_error),
            });

            return PageChrome("Ad Hoc Net", content, {{"F2", "Start Net"}, {"Esc", "Cancel"}});
        }

    private:
        AppState* state_;
        ftxui::Component input_name_;
        ftxui::Component input_mode_;
        ftxui::Component input_frequency_;
        ftxui::Component input_location_;
    };

    ftxui::Component BuildAdHocNetPage(AppState* state)
    {
        ftxui::Component input_name =
            ftxui::Input(&state->new_net_name, "e.g. Tailgate Net", SingleLineInputOption());
        ftxui::Component input_mode =
            ftxui::Input(&state->new_net_mode, "e.g. FM, SSB, Digital", SingleLineInputOption());
        ftxui::Component input_frequency =
            ftxui::Input(&state->new_net_frequency, "e.g. 146.940", SingleLineInputOption());
        ftxui::Component input_location =
            ftxui::Input(&state->new_net_location, "City, county, or ZIP", SingleLineInputOption());

        ftxui::Component root = ftxui::Container::Vertical({
            input_name,
            input_mode,
            input_frequency,
            input_location,
        });

        state->ad_hoc_net_name_input = input_name;

        return ftxui::Renderer(
            root, AdHocNetRenderer(state, input_name, input_mode, input_frequency, input_location));
    }

    // ---- Net history page ---------------------------------------------------

    class NetHistoryRenderer
    {
    public:
        NetHistoryRenderer(AppState* state, ftxui::Component instance_menu,
                           ftxui::Component checkin_menu)
            : state_(state),
              instance_menu_(std::move(instance_menu)),
              checkin_menu_(std::move(checkin_menu))
        {
        }

        ftxui::Element operator()() const
        {
            std::string net_name;
            if (state_->selected_net_index < static_cast<int>(state_->nets.size()))
            {
                net_name = state_->nets[state_->selected_net_index].name;
            }

            ftxui::Element instance_list =
                state_->history_instances.empty()
                    ? ftxui::text("No past instances of this net yet.") | ftxui::dim
                    : instance_menu_->Render() | ftxui::frame | ftxui::vscroll_indicator;

            ftxui::Element checkin_list =
                state_->history_check_in_labels.empty()
                    ? ftxui::text(state_->history_instances.empty() ? ""
                                                                    : "No check-ins were logged.") |
                          ftxui::dim
                    : checkin_menu_->Render() | ftxui::frame | ftxui::vscroll_indicator;

            ftxui::Element content = ftxui::vbox({
                (ftxui::vbox({
                     ftxui::text(FormatNetInstanceHeaderRow()) | ftxui::bold |
                         ftxui::color(ftxui::Color::Cyan),
                     instance_list,
                 }) |
                 ftxui::border) |
                    ftxui::flex,
                ftxui::separator(),
                (ftxui::vbox({
                     ftxui::text(FormatCheckInHeaderRow(/*above_menu=*/true)) | ftxui::bold |
                         ftxui::color(ftxui::Color::Cyan),
                     checkin_list,
                 }) |
                 ftxui::border) |
                    ftxui::flex,
                ErrorLine(state_->form_error),
            });

            return PageChrome("History: " + net_name, content,
                              {{"F5", "Delete Instance"}, {"Esc", "Back"}});
        }

    private:
        AppState* state_;
        ftxui::Component instance_menu_;
        ftxui::Component checkin_menu_;
    };

    ftxui::Component BuildNetHistoryPage(AppState* state)
    {
        ftxui::MenuOption instance_menu_option;
        instance_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        instance_menu_option.on_change = HistoryInstanceChangedHandler(state);
        ftxui::Component instance_menu = ftxui::Menu(
            &state->history_instance_labels, &state->selected_history_index, instance_menu_option);

        ftxui::MenuOption checkin_menu_option;
        checkin_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component checkin_menu =
            ftxui::Menu(&state->history_check_in_labels, &state->selected_history_check_in_index,
                        checkin_menu_option);

        ftxui::Component root = ftxui::Container::Vertical({instance_menu, checkin_menu});
        return ftxui::Renderer(root, NetHistoryRenderer(state, instance_menu, checkin_menu));
    }

    // ---- Edit net page ---------------------------------------------------

    class EditNetRenderer
    {
    public:
        EditNetRenderer(AppState* state, ftxui::Component input_name, ftxui::Component input_mode,
                        ftxui::Component input_frequency, ftxui::Component input_location,
                        ftxui::Component input_recurrence, ftxui::Component saved_station_menu,
                        StationFieldInputs saved_station_inputs,
                        ftxui::Component saved_station_suggestion_menu,
                        ftxui::Component saved_station_remarks_input)
            : state_(state),
              input_name_(std::move(input_name)),
              input_mode_(std::move(input_mode)),
              input_frequency_(std::move(input_frequency)),
              input_location_(std::move(input_location)),
              input_recurrence_(std::move(input_recurrence)),
              saved_station_menu_(std::move(saved_station_menu)),
              saved_station_inputs_(std::move(saved_station_inputs)),
              saved_station_suggestion_menu_(std::move(saved_station_suggestion_menu)),
              saved_station_remarks_input_(std::move(saved_station_remarks_input))
        {
        }

        ftxui::Element operator()() const
        {
            ftxui::Element saved_station_list =
                state_->edit_net_saved_stations.empty()
                    ? ftxui::text("No saved stations yet.") | ftxui::dim
                    : saved_station_menu_->Render() | ftxui::frame | ftxui::vscroll_indicator;

            ftxui::Element suggestions =
                state_->saved_station_suggestions.empty()
                    ? ftxui::text("")
                    : ftxui::vbox({
                          ftxui::text("Matches (Enter picks highlighted; Tab to browse):") |
                              ftxui::dim,
                          (ftxui::vbox({
                               ftxui::text(FormatCallsignSuggestionHeaderRow()) | ftxui::dim,
                               saved_station_suggestion_menu_->Render(),
                           }) |
                           ftxui::border),
                      });

            ftxui::Elements rows;
            rows.push_back(ftxui::hbox({FieldLabel("Name:       "), input_name_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Mode:       "), input_mode_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Frequency:  "), input_frequency_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Location:   "), input_location_->Render()}));
            rows.push_back(ftxui::hbox({FieldLabel("Recurrence: "), input_recurrence_->Render()}));
            rows.push_back(ftxui::separator());
            rows.push_back(ftxui::text("Saved Stations:") | ftxui::bold |
                           ftxui::color(ftxui::Color::Cyan));
            rows.push_back((ftxui::vbox({
                                ftxui::text(FormatSavedStationHeaderRow()) | ftxui::dim,
                                saved_station_list,
                            }) |
                            ftxui::border) |
                           ftxui::flex);
            rows.push_back(ftxui::text("Enter picks a station and jumps to its fields below.") |
                           ftxui::dim);
            rows.push_back(ftxui::separator());
            ftxui::Elements field_rows = StationFieldRows(saved_station_inputs_);
            rows.push_back(field_rows[0]);  // Callsign
            rows.push_back(suggestions);
            for (std::size_t i = 1; i < field_rows.size(); ++i)
            {
                rows.push_back(field_rows[i]);
            }
            rows.push_back(ftxui::hbox(
                {FieldLabel("Default Remarks: "), saved_station_remarks_input_->Render()}));
            rows.push_back(ErrorLine(state_->form_error));

            return PageChrome("Edit Net: " + state_->edit_net_name, ftxui::vbox(rows),
                              {
                                  {"F2", "Save Net"},
                                  {"F3", "Save Station"},
                                  {"F4", "Remove"},
                                  {"F5", "Delete"},
                                  {"F6", "Add Station"},
                                  {"Esc", "Cancel"},
                              });
        }

    private:
        AppState* state_;
        ftxui::Component input_name_;
        ftxui::Component input_mode_;
        ftxui::Component input_frequency_;
        ftxui::Component input_location_;
        ftxui::Component input_recurrence_;
        ftxui::Component saved_station_menu_;
        StationFieldInputs saved_station_inputs_;
        ftxui::Component saved_station_suggestion_menu_;
        ftxui::Component saved_station_remarks_input_;
    };

    ftxui::Component BuildEditNetPage(AppState* state)
    {
        ftxui::Component input_name =
            ftxui::Input(&state->edit_net_name, "e.g. Weekly Skywarn Net", SingleLineInputOption());
        ftxui::Component input_mode =
            ftxui::Input(&state->edit_net_mode, "e.g. FM, SSB, Digital", SingleLineInputOption());
        ftxui::Component input_frequency =
            ftxui::Input(&state->edit_net_frequency, "e.g. 146.940", SingleLineInputOption());
        ftxui::Component input_location = ftxui::Input(
            &state->edit_net_location, "City, county, or ZIP", SingleLineInputOption());
        ftxui::Component input_recurrence = ftxui::Input(
            &state->edit_net_recurrence, "e.g. Tuesdays 8pm ET", SingleLineInputOption());

        ftxui::MenuOption saved_station_menu_option;
        saved_station_menu_option.on_enter = LoadSavedStationHandler(state);
        saved_station_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component saved_station_menu =
            ftxui::Menu(&state->edit_net_saved_station_labels, &state->selected_saved_station_index,
                        saved_station_menu_option);

        ftxui::InputOption saved_station_callsign_option = SingleLineInputOption();
        saved_station_callsign_option.on_change = SavedStationCallsignChangeHandler(state);
        saved_station_callsign_option.on_enter = SavedStationCallsignEnterHandler(state);
        ftxui::Component saved_station_callsign_input =
            ftxui::Input(&state->saved_station.callsign, "Callsign", saved_station_callsign_option);
        state->saved_station_callsign_input = saved_station_callsign_input;
        StationFieldInputs saved_station_inputs =
            BuildStationFieldInputs(&state->saved_station, saved_station_callsign_input);
        ftxui::Component saved_station_remarks_input =
            ftxui::Input(&state->saved_station_remarks, "Optional", SingleLineInputOption());

        ftxui::MenuOption saved_station_suggestion_menu_option;
        saved_station_suggestion_menu_option.on_enter = SelectSavedStationSuggestionHandler(state);
        saved_station_suggestion_menu_option.entries_option.transform = AlignedMenuEntryTransform;
        ftxui::Component saved_station_suggestion_menu = ftxui::Menu(
            &state->saved_station_suggestion_labels,
            &state->selected_saved_station_suggestion_index, saved_station_suggestion_menu_option);

        ftxui::Components saved_station_field_components =
            StationFieldComponents(saved_station_inputs);
        saved_station_field_components.insert(saved_station_field_components.begin() + 1,
                                              saved_station_suggestion_menu);
        saved_station_field_components.push_back(saved_station_remarks_input);

        ftxui::Components root_components = {
            input_name,     input_mode,       input_frequency,
            input_location, input_recurrence, saved_station_menu,
        };
        for (const ftxui::Component& component : saved_station_field_components)
        {
            root_components.push_back(component);
        }

        ftxui::Component root = ftxui::Container::Vertical(root_components);

        state->edit_net_name_input = input_name;

        return ftxui::Renderer(
            root, EditNetRenderer(state, input_name, input_mode, input_frequency, input_location,
                                  input_recurrence, saved_station_menu, saved_station_inputs,
                                  saved_station_suggestion_menu, saved_station_remarks_input));
    }

}  // namespace ql
