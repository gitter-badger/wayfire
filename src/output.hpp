#ifndef OUTPUT_HPP
#define OUTPUT_HPP

#include "view.hpp"
#include "plugin.hpp"
#include <vector>
#include <unordered_map>
#include <pixman-1/pixman.h>
#include "../proto/wayfire-shell-server.h"

namespace OpenGL {
    struct context_t;
}
struct weston_seat;
struct weston_output;
struct weston_gl_renderer_api;
struct plugin_manager;

class workspace_manager;

/* Workspace streams are used if you need to continuously render a workspace
 * to a texture, for example if you call texture_from_viewport at every frame */
struct wf_workspace_stream {
    std::tuple<int, int> ws;
    uint fbuff, tex;
    bool running = false;

    float scale_x, scale_y;
};

struct render_manager {
    private:
        wayfire_output *output;
        int constant_redraw = 0;

        bool dirty_context = true;

        void load_context();
        void release_context();

        render_hook_t renderer;

        pixman_region32_t frame_damage, prev_damage;
        int streams_running = 0;

    public:
        OpenGL::context_t *ctx;
    	static const weston_gl_renderer_api *renderer_api;

        render_manager(wayfire_output *o);

        void set_renderer(render_hook_t rh = nullptr);

        void auto_redraw(bool redraw); /* schedule repaint immediately after finishing the last */
        void transformation_renderer();
        void reset_renderer();

        void paint(pixman_region32_t *damage);
        void run_effects();

        std::vector<effect_hook_t*> output_effects;
        void add_output_effect(effect_hook_t*, wayfire_view v = nullptr);
        void rem_effect(const effect_hook_t*, wayfire_view v = nullptr);

        /* this function renders a viewport and
         * saves the image in texture which is returned */
        void texture_from_workspace(std::tuple<int, int>, uint& fbuff, uint &tex);

        void workspace_stream_start(wf_workspace_stream *stream);
        void workspace_stream_update(wf_workspace_stream *stream,
                float scale_x = 1, float scale_y = 1);
        void workspace_stream_stop(wf_workspace_stream *stream);
};

struct wf_workspace_implementation
{
    virtual bool view_movable(wayfire_view view) = 0;
    virtual bool view_resizable(wayfire_view view) = 0;
};

class workspace_manager
{
    public:
        /* we could actually attach signal listeners, but this is easier */
        virtual void view_bring_to_front(wayfire_view view) = 0;
        virtual void view_removed(wayfire_view view) = 0;

        /* return if the view is visible on the given workspace */
        virtual bool view_visible_on(wayfire_view view, std::tuple<int, int>) = 0;

        virtual void for_each_view(view_callback_proc_t call) = 0;
        virtual void for_each_view_reverse(view_callback_proc_t call) = 0;

        /* return the active wf_workspace_implementation for the given workpsace */
        virtual wf_workspace_implementation* get_implementation(std::tuple<int, int>) = 0;
        /* returns true if implementation of workspace has been successfully installed.
         * @param override - override current implementation if it is existing.
         * it must be guaranteed that if override is set, then the functions returns true */
        virtual bool set_implementation(std::tuple<int, int>, wf_workspace_implementation *, bool override = false) = 0;

        /* toplevel views (i.e windows) on the given workspace */
        virtual std::vector<wayfire_view>
            get_views_on_workspace(std::tuple<int, int>) = 0;

        virtual void set_workspace(std::tuple<int, int>) = 0;
        virtual std::tuple<int, int> get_current_workspace() = 0;
        virtual std::tuple<int, int> get_workspace_grid_size() = 0;

        virtual wayfire_view get_background_view() = 0;

        /* returns a list of all views on workspace that are visible on the current
         * workspace except panels(but should include background)
         * The list must be returned from top to bottom(i.e the last is background) */
        virtual std::vector<wayfire_view>
            get_renderable_views_on_workspace(std::tuple<int, int> ws) = 0;

        /* wayfire_shell implementation */
        virtual void add_background(wayfire_view background, int x, int y) = 0;
        virtual void add_panel(wayfire_view panel) = 0;
        virtual void reserve_workarea(wayfire_shell_panel_position position,
                uint32_t width, uint32_t height) = 0;
        virtual void configure_panel(wayfire_view view, int x, int y) = 0;

        /* returns the available area for views, it is basically
         * the output geometry minus the area reserved for panels */
        virtual weston_geometry get_workarea() = 0;
};

struct signal_manager
{
    private:
        std::unordered_map<std::string, std::vector<signal_callback_t*>> sig;
    public:
        void connect_signal(std::string name, signal_callback_t* callback);
        void disconnect_signal(std::string name, signal_callback_t* callback);
        void emit_signal(std::string name, signal_data *data);
};

class wayfire_output
{
    friend class wayfire_core;

    private:
       std::unordered_multiset<wayfire_grab_interface> active_plugins;
       plugin_manager *plugin;

       wayfire_view active_view;

       /* return an active wayfire_grab_interface on this output
        * which has grabbed the input. If none, then return nullptr */
       wayfire_grab_interface get_input_grab_interface();

       wl_listener destroy_listener;

    public:
    weston_output* handle;


    /* used for differences between backends */
    int output_dx, output_dy;
    std::tuple<int, int> get_screen_size();

    render_manager *render;
    signal_manager *signal;
    workspace_manager *workspace;

    wayfire_output(weston_output*, wayfire_config *config);
    ~wayfire_output();
    weston_geometry get_full_geometry();

    void set_transform(wl_output_transform new_transform);
    wl_output_transform get_transform();
    /* makes sure that the pointer is inside the output's geometry */
    void ensure_pointer();

    /* @param break_fs - lower fullscreen windows if any */
    bool activate_plugin  (wayfire_grab_interface owner, bool lower_fs = true);
    bool deactivate_plugin(wayfire_grab_interface owner);
    bool is_plugin_active (owner_t owner_name);

    void activate();
    void deactivate();

    wayfire_view get_top_view();
    wayfire_view get_view_at_point(int x, int y);

    void attach_view(wayfire_view v);
    void detach_view(wayfire_view v);

    void focus_view(wayfire_view v, weston_seat *seat = nullptr);
    void set_active_view(wayfire_view v);
    void bring_to_front(wayfire_view v);

    weston_binding *add_key(uint32_t mod, uint32_t key, key_callback *);
    weston_binding *add_button(uint32_t mod, uint32_t button, button_callback *);

    int add_touch(uint32_t mod, touch_callback*);
    void rem_touch(int32_t id);

    /* we take only gesture type and finger count into account,
     * we send for all possible directions */
    int add_gesture(const wayfire_touch_gesture& gesture, touch_gesture_callback* callback);
    void rem_gesture(int id);
};
extern const struct wayfire_shell_interface shell_interface_impl;
#endif /* end of include guard: OUTPUT_HPP */
