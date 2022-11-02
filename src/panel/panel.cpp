#include <glibmm/main.h>
#include <gtkmm/window.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/hvbox.h>
#include <gtkmm/application.h>
#include <gdk/gdkwayland.h>
#include <gtk-layer-shell.h>

#include <stdio.h>
#include <iostream>
#include <sstream>

#include <map>

#include "panel.hpp"
#include "../util/gtk-utils.hpp"

#include "widgets/battery.hpp"
#include "widgets/menu.hpp"
#include "widgets/clock.hpp"
#include "widgets/launchers.hpp"
#include "widgets/network.hpp"
#include "widgets/spacing.hpp"
#ifdef HAVE_PULSE
#include "widgets/volume.hpp"
#endif
#include "widgets/volumepulse.hpp"
#include "widgets/micpulse.hpp"
#include "widgets/smenu.hpp"
#include "widgets/netman.hpp"
#include "widgets/bluetooth.hpp"
#include "widgets/ejecter.hpp"
#include "widgets/updater.hpp"
#include "widgets/window-list/window-list.hpp"
#include "widgets/notifications/notification-center.hpp"

#include "wf-autohide-window.hpp"

/* Minimal DBus interface for commands to plugins */

static GDBusNodeInfo *introspection_data = NULL;

static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='org.wayfire.wfpanel'>"
  "    <annotation name='org.wayfire.wfpanel.Annotation' value='OnInterface'/>"
  "    <method name='command'>"
  "      <annotation name='org.wayfire.wfpanel.Annotation' value='OnMethod'/>"
  "      <arg type='s' name='plugin' direction='in'/>"
  "      <arg type='s' name='command' direction='in'/>"
  "    </method>"
  "  </interface>"
  "</node>";

const GDBusInterfaceVTable WayfirePanelApp::interface_vtable =
{
  handle_method_call,
  handle_get_property,
  handle_set_property
};

void WayfirePanelApp::handle_method_call (GDBusConnection *connection, const gchar *sender, const gchar *object_path, const gchar *interface_name,
    const gchar *method_name, GVariant *parameters, GDBusMethodInvocation *invocation, gpointer user_data)
{
    if (g_strcmp0 (method_name, "command") == 0)
    {
        const gchar *plugin, *command;
        g_variant_get (parameters, "(&s&s)", &plugin, &command);
        get().on_command (plugin, command);
    }
}

GVariant *WayfirePanelApp::handle_get_property (GDBusConnection *connection, const gchar *sender, const gchar *object_path, const gchar *interface_name,
    const gchar *property_name, GError **error, gpointer user_data)
{
    return NULL;
}

gboolean WayfirePanelApp::handle_set_property (GDBusConnection *connection, const gchar *sender, const gchar *object_path, const gchar *interface_name,
    const gchar *property_name, GVariant *value, GError **error, gpointer user_data)
{
    return TRUE;
}

void WayfirePanelApp::on_bus_acquired (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    g_dbus_connection_register_object (connection, "/org/wayfire/wfpanel", introspection_data->interfaces[0],
        &interface_vtable, user_data, NULL, NULL);
}

void WayfirePanelApp::on_name_acquired (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
}

void WayfirePanelApp::on_name_lost (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
}


struct WayfirePanelZwfOutputCallbacks
{
    std::function<void()> enter_fullscreen;
    std::function<void()> leave_fullscreen;
};

static void handle_zwf_output_enter_fullscreen(void* dd,
    zwf_output_v2 *zwf_output_v2)
{
    auto data = (WayfirePanelZwfOutputCallbacks*)
        zwf_output_v2_get_user_data(zwf_output_v2);

    if (data)
        data->enter_fullscreen();
}

static void handle_zwf_output_leave_fullscreen(void *,
    zwf_output_v2 *zwf_output_v2)
{
    auto data = (WayfirePanelZwfOutputCallbacks*)
        zwf_output_v2_get_user_data(zwf_output_v2);

    if (data)
        data->leave_fullscreen();
}

static struct zwf_output_v2_listener output_impl = {
    .enter_fullscreen = handle_zwf_output_enter_fullscreen,
    .leave_fullscreen = handle_zwf_output_leave_fullscreen,
};

class WayfirePanel::impl
{
    std::unique_ptr<WayfireAutohidingWindow> window;

    Gtk::HBox content_box;
    Gtk::HBox left_box, center_box, right_box;

    using Widget = std::unique_ptr<WayfireWidget>;
    using WidgetContainer = std::vector<Widget>;
    WidgetContainer left_widgets, center_widgets, right_widgets;

    WayfireOutput *output;

    int last_autohide_value = -1;
    WfOption<bool> autohide_opt{"panel/autohide"};
    std::function<void()> autohide_opt_updated = [=] ()
    {
        if (autohide_opt == last_autohide_value)
            return;

        if (autohide_opt) {
            this->window->increase_autohide();
        } else if (last_autohide_value == 1) {
            this->window->decrease_autohide();
        }

        last_autohide_value = autohide_opt;
        window->set_auto_exclusive_zone(!autohide_opt);
    };

#if 0
    WfOption<std::string> bg_color{"panel/background_color"};
    std::function<void()> on_window_color_updated = [=] ()
    {
        if ((std::string)bg_color == "gtk_default")
            return window->unset_background_color();

        Gdk::RGBA rgba;
        if ((std::string)bg_color == "gtk_headerbar")
        {
            Gtk::HeaderBar headerbar;
            rgba = headerbar.get_style_context()->get_background_color();
        } else {
            auto color = wf::option_type::from_string<wf::color_t> (
                ((wf::option_sptr_t<std::string>)bg_color)->get_value_str());
            if (!color) {
                std::cerr << "Invalid panel background color in"
                    " config file" << std::endl;
                return;
            }

            rgba.set_red(color.value().r);
            rgba.set_green(color.value().g);
            rgba.set_blue(color.value().b);
            rgba.set_alpha(color.value().a);
        }

        window->override_background_color(rgba);
    };

    WfOption<std::string> panel_layer{"panel/layer"};
    std::function<void()> set_panel_layer = [=] ()
    {
        if ((std::string)panel_layer == "overlay")
            gtk_layer_set_layer(window->gobj(), GTK_LAYER_SHELL_LAYER_OVERLAY);
        if ((std::string)panel_layer == "top")
            gtk_layer_set_layer(window->gobj(), GTK_LAYER_SHELL_LAYER_TOP);
        if ((std::string)panel_layer == "bottom")
            gtk_layer_set_layer(window->gobj(), GTK_LAYER_SHELL_LAYER_BOTTOM);
        if ((std::string)panel_layer == "background")
            gtk_layer_set_layer(window->gobj(), GTK_LAYER_SHELL_LAYER_BACKGROUND);
    };
#endif

    WfOption<int> minimal_panel_height{"panel/minimal_height"};
    WfOption<std::string> css_path{"panel/css_path"};

    void create_window()
    {
        window = std::make_unique<WayfireAutohidingWindow> (output, "panel");
        window->set_size_request(1, minimal_panel_height);
        //panel_layer.set_callback(set_panel_layer);
        //set_panel_layer(); // initial setting

        gtk_layer_set_anchor(window->gobj(), GTK_LAYER_SHELL_EDGE_LEFT, true);
        gtk_layer_set_anchor(window->gobj(), GTK_LAYER_SHELL_EDGE_RIGHT, true);
        gtk_layer_set_keyboard_mode (window->gobj(), GTK_LAYER_SHELL_KEYBOARD_MODE_ON_DEMAND);
        gtk_layer_set_layer(window->gobj(), GTK_LAYER_SHELL_LAYER_TOP);

        gtk_widget_set_name (GTK_WIDGET (window->gobj()), "PanelToplevel");

        //bg_color.set_callback(on_window_color_updated);
        //on_window_color_updated(); // set initial color

        autohide_opt.set_callback(autohide_opt_updated);
        autohide_opt_updated(); // set initial autohide status

        if ((std::string)css_path != "")
        {
            auto css = load_css_from_path(css_path);
            if (css)
            {
                auto screen = Gdk::Screen::get_default();
                auto style_context = Gtk::StyleContext::create();
                style_context->add_provider_for_screen(
                    screen, css, GTK_STYLE_PROVIDER_PRIORITY_USER);
            }
        }

        window->show_all();
        init_widgets();
        init_layout();

        window->signal_delete_event().connect(
            sigc::mem_fun(this, &WayfirePanel::impl::on_delete));
    }

    bool on_delete(GdkEventAny *ev)
    {
        /* We ignore close events, because the panel's lifetime is bound to
         * the lifetime of the output */
        return true;
    }

    void init_layout()
    {
        content_box.pack_start(left_box, false, false);
        std::vector<Gtk::Widget*> center_children = center_box.get_children();
        if (center_children.size() > 0)
            content_box.set_center_widget(center_box);
        content_box.pack_end(right_box, false, false);
        window->add(content_box);
        window->show_all();
    }

    Widget widget_from_name(std::string name)
    {
        if (name == "menu")
            return Widget(new WayfireMenu());
        if (name == "launchers")
            return Widget(new WayfireLaunchers());
        if (name == "clock")
            return Widget(new WayfireClock());
        if (name == "network")
            return Widget(new WayfireNetworkInfo());
        if (name == "battery")
            return Widget(new WayfireBatteryInfo());
        if (name == "volume") {
#ifdef HAVE_PULSE
            return Widget(new WayfireVolume());
#else
#warning "Pulse not found, volume widget will not be available."
        std::cerr << "Built without pulse support, volume widget "
            " is not available." << std::endl;
#endif
        }
        if (name == "volumepulse")
            return Widget(new WayfireVolumepulse());
        if (name == "micpulse")
            return Widget(new WayfireMicpulse());
        if (name == "smenu")
            return Widget(new WayfireSmenu());
        if (name == "netman")
            return Widget(new WayfireNetman());
        if (name == "bluetooth")
            return Widget(new WayfireBluetooth());
        if (name == "ejecter")
            return Widget(new WayfireEjecter());
        if (name == "updater")
            return Widget(new WayfireUpdater());
        if (name == "window-list")
            return Widget(new WayfireWindowList(output));
        if (name == "notifications")
            return Widget(new WayfireNotificationCenter());

        std::string spacing = "spacing";
        if (name.find(spacing) == 0)
        {
            auto pixel_str = name.substr(spacing.size());
            int pixel = std::atoi(pixel_str.c_str());

            if (pixel <= 0)
            {
                std::cerr << "Invalid spacing, " << pixel << std::endl;
                return nullptr;
            }

            return Widget(new WayfireSpacing(pixel));
        }

        if (name != "none")
            std::cerr << "Invalid widget: " << name << std::endl;
        return nullptr;
    }

    static std::vector<std::string> tokenize(std::string list)
    {
        std::string token;
        std::istringstream stream(list);
        std::vector<std::string> result;

        while(stream >> token)
        {
            if (token.size())
                result.push_back(token);
        }

        return result;
    }

    void reload_widgets(std::string list, WidgetContainer& container,
                        Gtk::HBox& box)
    {
        container.clear();
        auto widgets = tokenize(list);
        for (auto widget_name : widgets)
        {
            auto widget = widget_from_name(widget_name);
            if (!widget)
                continue;

            widget->widget_name = widget_name;
            widget->init(&box);
            container.push_back(std::move(widget));
        }
    }

    WfOption<std::string> left_widgets_opt{"panel/widgets_left"};
    WfOption<std::string> right_widgets_opt{"panel/widgets_right"};
    WfOption<std::string> center_widgets_opt{"panel/widgets_center"};
    void init_widgets()
    {
        left_widgets_opt.set_callback([=] () {
            reload_widgets((std::string)left_widgets_opt, left_widgets, left_box);
        });
        right_widgets_opt.set_callback([=] () {
            reload_widgets((std::string)right_widgets_opt, right_widgets, right_box);
        });
        center_widgets_opt.set_callback([=] () {
            reload_widgets((std::string)center_widgets_opt, center_widgets, center_box);
        });

        reload_widgets((std::string)left_widgets_opt, left_widgets, left_box);
        reload_widgets((std::string)right_widgets_opt, right_widgets, right_box);
        reload_widgets((std::string)center_widgets_opt, center_widgets, center_box);
    }

    WayfirePanelZwfOutputCallbacks callbacks;

    WfOption <int> notify_timeout {"panel/notify_timeout"};
    WfOption <bool> notifications {"panel/notifications"};

    public:
    impl(WayfireOutput *output)
    {
        this->output = output;
        create_window();

        if (output->output)
        {
            callbacks.enter_fullscreen = [=]() { window->increase_autohide(); };
            callbacks.leave_fullscreen = [=]() { window->decrease_autohide(); };
            zwf_output_v2_add_listener(output->output, &output_impl, NULL);
            zwf_output_v2_set_user_data(output->output, &callbacks);
        }
        lxpanel_notify_init (notifications, notify_timeout);
    }

    ~impl()
    {
        if (output->output)
            zwf_output_v2_set_user_data(output->output, NULL);
    }

    wl_surface *get_wl_surface()
    {
        return window->get_wl_surface();
    }

    Gtk::Window& get_window()
    {
        return *window;
    }

    void handle_config_reload()
    {
        lxpanel_notify_init (notifications, notify_timeout);
        for (auto& w : left_widgets)
            w->handle_config_reload();
        for (auto& w : right_widgets)
            w->handle_config_reload();
        for (auto& w : center_widgets)
            w->handle_config_reload();
    }

    void message_widget (const char *name, const char *cmd)
    {
        if (!g_strcmp0 (name, "notify"))
        {
            lxpanel_notify (cmd);
            return;
        }

        for (auto& w : left_widgets)
            if (name == w->widget_name) w->command (cmd);
        for (auto& w : right_widgets)
            if (name == w->widget_name) w->command (cmd);
        for (auto& w : center_widgets)
            if (name == w->widget_name) w->command (cmd);
    }
};

WayfirePanel::WayfirePanel(WayfireOutput *output) : pimpl(new impl(output)) { }
wl_surface *WayfirePanel::get_wl_surface() { return pimpl->get_wl_surface(); }
Gtk::Window& WayfirePanel::get_window() { return pimpl->get_window(); }
void WayfirePanel::handle_config_reload() { return pimpl->handle_config_reload(); }
void WayfirePanel::handle_command_message (const char *plugin, const char *cmd) { pimpl->message_widget (plugin, cmd); }

class WayfirePanelApp::impl
{
  public:
    std::map<WayfireOutput*, std::unique_ptr<WayfirePanel> > panels;
};

void WayfirePanelApp::on_config_reload()
{
    for (auto& p : priv->panels)
        p.second->handle_config_reload();
}

void WayfirePanelApp::on_command (const char *plugin, const char *command)
{
    for (auto& p : priv->panels)
        p.second->handle_command_message (plugin, command);
}

void WayfirePanelApp::handle_new_output(WayfireOutput *output)
{
    priv->panels[output] = std::unique_ptr<WayfirePanel> (
        new WayfirePanel(output));
}

WayfirePanel* WayfirePanelApp::panel_for_wl_output(wl_output *output)
{
    for (auto& p : priv->panels)
    {
        if (p.first->wo == output)
            return p.second.get();
    }

    return nullptr;
}

void WayfirePanelApp::handle_output_removed(WayfireOutput *output)
{
    priv->panels.erase(output);
}

WayfirePanelApp& WayfirePanelApp::get()
{
    if (!instance)
        throw std::logic_error("Calling WayfirePanelApp::get() before starting app!");
    return dynamic_cast<WayfirePanelApp&>(*instance.get());
}

void WayfirePanelApp::create(int argc, char **argv)
{
    if (instance)
        throw std::logic_error("Running WayfirePanelApp twice!");

    introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, NULL);
    guint owner_id = g_bus_own_name (G_BUS_TYPE_SESSION, "org.wayfire.wfpanel", G_BUS_NAME_OWNER_FLAGS_NONE,
        on_bus_acquired, on_name_acquired, on_name_lost, NULL, NULL);

    instance = std::unique_ptr<WayfireShellApp>(new WayfirePanelApp{argc, argv});
    instance->run();

    g_bus_unown_name (owner_id);
    g_dbus_node_info_unref (introspection_data);
}

WayfirePanelApp::~WayfirePanelApp() = default;
WayfirePanelApp::WayfirePanelApp(int argc, char **argv)
    : WayfireShellApp(argc, argv), priv(new impl()) { }

int main(int argc, char **argv)
{
    WayfirePanelApp::create(argc, argv);

    return 0;
}
