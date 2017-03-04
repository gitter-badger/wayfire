#ifndef FIRE_H
#define FIRE_H

#include "plugin.hpp"
#include <vector>
#include <map>

using output_callback_proc = std::function<void(wayfire_output*)>;

class wayfire_core {
    friend class plugin_manager;

    uint32_t nextID = 0;
    wayfire_config *config;

    wayfire_output *active_output;
    std::map<uint32_t, wayfire_output*> outputs;
    std::map<weston_view*, wayfire_view> views;

    void configure(wayfire_config *config);

    public:
    std::string wayland_display, xwayland_display;

    weston_compositor *ec;
    void init(weston_compositor *ec, wayfire_config *config);

    weston_seat *get_current_seat();

    void add_view(weston_desktop_surface*);
    wayfire_view find_view(weston_view*);
    wayfire_view find_view(weston_desktop_surface*);
    /* Only removes the view from the "database".
     * Use only when view is already destroyed and detached from output */
    void erase_view(wayfire_view view);

    /* brings the view to the top
     * and also focuses its output */
    void focus_view(wayfire_view win, weston_seat* seat);
    void close_view(wayfire_view win);
    void move_view_to_output(wayfire_view v, wayfire_output *old, wayfire_output *new_output);

    void add_output(weston_output *output);
    wayfire_output* get_output(weston_output *output);

    void focus_output(wayfire_output* o);
    void remove_output(wayfire_output* o);

    wayfire_output *get_active_output();
    wayfire_output *get_next_output();

    void for_each_output(output_callback_proc);

    void run(const char *command);

    int vwidth, vheight;

    std::string background, shadersrc, plugin_path, plugins;
};

extern wayfire_core *core;
#endif
