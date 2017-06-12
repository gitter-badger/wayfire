#include "opengl.hpp"
#include "output.hpp"
#include "signal_definitions.hpp"
#include <linux/input.h>

#include "wm.hpp"

#include <sstream>
#include <memory>
#include <dlfcn.h>
#include <algorithm>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <libweston-3/libweston-desktop.h>

/* Start plugin manager */
plugin_manager::plugin_manager(wayfire_output *o, wayfire_config *config)
{
    init_default_plugins();
    load_dynamic_plugins();

    for (auto p : plugins) {
        p->grab_interface = new wayfire_grab_interface_t(o);
        p->output = o;

        p->init(config);
    }
}

plugin_manager::~plugin_manager()
{
    for (auto p : plugins) {
        p->fini();
        delete p->grab_interface;

        if (p->dynamic)
            dlclose(p->handle);
        p.reset();
    }
}

namespace
{
template<class A, class B> B union_cast(A object)
{
    union {
        A x;
        B y;
    } helper;
    helper.x = object;
    return helper.y;
}
}

wayfire_plugin plugin_manager::load_plugin_from_file(std::string path, void **h)
{
    void *handle = dlopen(path.c_str(), RTLD_NOW);
    if(handle == NULL) {
        errio << "Can't load plugin " << path << std::endl;
        errio << "\t" << dlerror() << std::endl;
        return nullptr;
    }

    debug << "Loading plugin " << path << std::endl;

    auto initptr = dlsym(handle, "newInstance");
    if(initptr == NULL) {
        errio << "Missing function newInstance in file " << path << std::endl;
        errio << dlerror();
        return nullptr;
    }
    get_plugin_instance_t init = union_cast<void*, get_plugin_instance_t> (initptr);
    *h = handle;
    return wayfire_plugin(init());
}

void plugin_manager::load_dynamic_plugins()
{
    std::stringstream stream(core->plugins);
    auto path = core->plugin_path + "/wayfire/";

    std::string plugin;
    while(stream >> plugin) {
        if(plugin != "") {
            void *handle;
            auto ptr = load_plugin_from_file(path + "/lib" + plugin + ".so", &handle);
            if(ptr) {
                ptr->handle  = handle;
                ptr->dynamic = true;
                plugins.push_back(ptr);
            }
        }
    }
}

template<class T>
wayfire_plugin plugin_manager::create_plugin()
{
    return std::static_pointer_cast<wayfire_plugin_t>(std::make_shared<T>());
}

void plugin_manager::init_default_plugins()
{
    plugins.push_back(create_plugin<wayfire_focus>());
    plugins.push_back(create_plugin<wayfire_close>());
    plugins.push_back(create_plugin<wayfire_exit>());
}

/* End plugin_manager */

/* Start render_manager */
render_manager::render_manager(wayfire_output *o)
{
    output = o;

    if (core->backend == WESTON_BACKEND_WAYLAND) {
        debug << "Yes, try it" << std::endl;
        output->output_dx = output->output_dy = 38;
    } else {
        output->output_dx = output->output_dy = 0;
    }
}

void render_manager::load_context()
{
    ctx = OpenGL::create_gles_context(output, core->shadersrc.c_str());
    OpenGL::bind_context(ctx);

    dirty_context = false;

    output->signal->emit_signal("reload-gl", nullptr);
}

void render_manager::release_context()
{
    OpenGL::release_context(ctx);
    dirty_context = true;
}

void redraw_idle_cb(void *data)
{
    wayfire_output *output = (wayfire_output*) data;
    assert(output);

    weston_output_schedule_repaint(output->handle);
}

void render_manager::auto_redraw(bool redraw)
{
    if (redraw == constant_redraw) /* no change, exit */
        return;

    constant_redraw = redraw;
    auto loop = wl_display_get_event_loop(core->ec->wl_display);
    wl_event_loop_add_idle(loop, redraw_idle_cb, output);
}

void render_manager::reset_renderer()
{
    renderer = nullptr;

    weston_output_damage(output->handle);
    weston_output_schedule_repaint(output->handle);
}

void render_manager::set_renderer(render_hook_t rh)
{
    if (!rh) {
        renderer = std::bind(std::mem_fn(&render_manager::transformation_renderer), this);
    } else {
        renderer = rh;
    }
}

struct weston_gl_renderer {
    weston_renderer base;
    int a, b;
    void *c, *d;
    EGLDisplay display;
    EGLContext context;
};

void render_manager::paint(pixman_region32_t *damage)
{
    if (dirty_context)
        load_context();

    // This is a hack, weston renderer_state is a struct and the EGLSurface is the first field
    // In the future this might change so we need to track changes in weston
    EGLSurface surf = *(EGLSurface*)output->handle->renderer_state;
    weston_gl_renderer *gr = (weston_gl_renderer*) core->ec->renderer;
    eglMakeCurrent(gr->display, surf, surf, gr->context);

    GL_CALL(glViewport(output->handle->x, output->handle->y,
                output->handle->width, output->handle->height));

    if (renderer) {
        OpenGL::bind_context(ctx);
        renderer();

        wl_signal_emit(&output->handle->frame_signal, output->handle);
        eglSwapBuffers(gr->display, surf);
    } else {
        core->weston_repaint(output->handle, damage);
    }

    if (constant_redraw) {
        wl_event_loop_add_idle(wl_display_get_event_loop(core->ec->wl_display),
                redraw_idle_cb, output);
    }
}

void render_manager::pre_paint()
{
    std::vector<effect_hook_t*> active_effects;
    for (auto effect : output_effects) {
        active_effects.push_back(effect);
    }

    for (auto& effect : active_effects)
        (*effect)();
}

void render_manager::transformation_renderer()
{
    auto bg = output->workspace->get_background_view();
    if (bg)
        bg->render(0);

    output->workspace->for_each_view_reverse([=](wayfire_view v) {
        if (!v->destroyed && !v->is_hidden)
            v->render();
    });
}

void render_manager::add_output_effect(effect_hook_t* hook, wayfire_view v)
{
    if (v)
        v->effects.push_back(hook);
    else
        output_effects.push_back(hook);
}

void render_manager::rem_effect(const effect_hook_t *hook, wayfire_view v)
{
    decltype(output_effects)& container = output_effects;
    if (v) container = v->effects;
    auto it = std::remove_if(container.begin(), container.end(),
    [hook] (const effect_hook_t *h) {
        if (h == hook)
            return true;
        return false;
    });

    container.erase(it, container.end());
}
/* End render_manager */

/* Start SignalManager */

void signal_manager::connect_signal(std::string name, signal_callback_t* callback)
{
    sig[name].push_back(callback);
}

void signal_manager::disconnect_signal(std::string name, signal_callback_t* callback)
{
    auto it = std::remove_if(sig[name].begin(), sig[name].end(),
    [=] (const signal_callback_t *call) {
        return call == callback;
    });

    sig[name].erase(it, sig[name].end());
}

void signal_manager::emit_signal(std::string name, signal_data *data)
{
    std::vector<signal_callback_t> callbacks;
    for (auto x : sig[name])
        callbacks.push_back(*x);

    for (auto x : callbacks)
        x(data);
}

/* End SignalManager */
/* Start output */
wayfire_output* wl_output_to_wayfire_output(uint32_t output)
{
    wayfire_output *result = nullptr;
    core->for_each_output([output, &result] (wayfire_output *wo) {
        if (wo->handle->id == output)
            result = wo;
    });

    return result;
}

void shell_add_background(struct wl_client *client, struct wl_resource *resource,
        uint32_t output, struct wl_resource *surface, int32_t x, int32_t y)
{
    weston_surface *wsurf = (weston_surface*) wl_resource_get_user_data(surface);
    auto wo = wl_output_to_wayfire_output(output);

    wayfire_view view = nullptr;

    if (!wo || !wsurf || !(view = core->find_view(wsurf))) {
        errio << "shell_add_background called with invalid surface or output" << std::endl;
        return;
    }

    debug << "wf_shell: add_background" << std::endl;
    wo->workspace->add_background(view, x, y);
}

void shell_add_panel(struct wl_client *client, struct wl_resource *resource,
        uint32_t output, struct wl_resource *surface)
{
    weston_surface *wsurf = (weston_surface*) wl_resource_get_user_data(surface);
    auto wo = wl_output_to_wayfire_output(output);

    wayfire_view view = nullptr;

    if (!wo || !wsurf || !(view = core->find_view(wsurf))) {
        errio << "shell_add_panel called with invalid surface or output" << std::endl;
        return;
    }

    debug << "wf_shell: add_panel" << std::endl;
    wo->workspace->add_panel(view);
}

void shell_configure_panel(struct wl_client *client, struct wl_resource *resource,
        uint32_t output, struct wl_resource *surface, int32_t x, int32_t y)
{
    weston_surface *wsurf = (weston_surface*) wl_resource_get_user_data(surface);
    auto wo = wl_output_to_wayfire_output(output);

    wayfire_view view = nullptr;

    if (!wo || !wsurf || !(view = core->find_view(wsurf))) {
        errio << "shell_configure_panel called with invalid surface or output" << std::endl;
        return;
    }

    debug << "wf_shell: configure_panel" << std::endl;
    wo->workspace->configure_panel(view, x, y);
}

void shell_reserve(struct wl_client *client, struct wl_resource *resource,
        uint32_t output, uint32_t side, uint32_t width, uint32_t height)
{
    auto wo = wl_output_to_wayfire_output(output);

    if (!wo) {
        errio << "shell_reserve called with invalid output" << std::endl;
        return;
    }

    debug << "wf_shell: reserve" << std::endl;
    wo->workspace->reserve_workarea((wayfire_shell_panel_position)side, width, height);
}

void shell_set_color_gamma(wl_client *client, wl_resource *res,
        uint32_t output, wl_array *r, wl_array *g, wl_array *b)
{
    auto wo = wl_output_to_wayfire_output(output);
    if (!wo || !wo->handle->set_gamma) {
        errio << "shell_set_gamma called with invalid/unsupported output" << std::endl;
        return;
    }

    size_t size = wo->handle->gamma_size * sizeof(uint16_t);
    if (r->size != size || b->size != size || g->size != size) {
        errio << "gamma size is not equal to output's gamma size " << r->size << " " << size << std::endl;
        return;
    }

    size /= sizeof(uint16_t);
#ifndef ushort
#define ushort unsigned short
    wo->handle->set_gamma(wo->handle, size, (ushort*)r->data, (ushort*)g->data, (ushort*)b->data);
#undef ushort
#endif
}

const struct wayfire_shell_interface shell_interface_impl {
    .add_background = shell_add_background,
    .add_panel = shell_add_panel,
    .configure_panel = shell_configure_panel,
    .reserve = shell_reserve,
    .set_color_gamma = shell_set_color_gamma
};

wayfire_output::wayfire_output(weston_output *handle, wayfire_config *c)
{
    this->handle = handle;
    this->transform = (wl_output_transform)handle->transform;

    render = new render_manager(this);
    signal = new signal_manager();

    plugin = new plugin_manager(this, c);

    weston_output_damage(handle);
    weston_output_schedule_repaint(handle);
}

wayfire_output::~wayfire_output()
{
    delete plugin;
    delete signal;
    delete render;
}

wayfire_geometry wayfire_output::get_full_geometry()
{
    return {.origin = {handle->x, handle->y},
            .size = {handle->width, handle->height}};
}

void wayfire_output::set_transform(wl_output_transform new_tr)
{
    /* TODO: this doesn't work, figure out some way to "restart" output */
    transform = new_tr;

    int w = render->ctx->device_width;
    int h = render->ctx->device_height;

    int old_w = handle->width;
    int old_h = handle->height;

    switch(new_tr) {
        case WL_OUTPUT_TRANSFORM_90:
        case WL_OUTPUT_TRANSFORM_270:
            std::swap(w, h);
            break;
        default:
            break;
    }

    handle->width = w;
    handle->height = h;

    render->ctx->width = w;
    render->ctx->height = h;

    pixman_region32_fini(&handle->previous_damage);
    pixman_region32_init(&handle->previous_damage);

    pixman_region32_fini(&handle->region);
    pixman_region32_init_rect(&handle->region, handle->x, handle->y, w, h);

    handle->dirty = 1;
    handle->transform = new_tr;

    wl_resource *resource;
    wl_resource_for_each(resource, &handle->resource_list) {
        wl_output_send_geometry(resource, handle->x, handle->y,
                handle->mm_width, handle->mm_height,
                handle->subpixel, handle->make, handle->model,
                new_tr);

        if (wl_resource_get_version(resource) >= WL_OUTPUT_DONE_SINCE_VERSION)
            wl_output_send_done(resource);
    }

    weston_output_damage(handle);

    wayfire_shell_send_output_resized(core->wf_shell.resource, handle->id, w, h);
    signal->emit_signal("output-resized", nullptr);

    ensure_pointer();

    workspace->for_each_view([=] (wayfire_view view) {
        if (view->fullscreen) {
            view->set_geometry(get_full_geometry());
        } else if (view->maximized) {
            view->set_geometry(workspace->get_workarea());
        } else {
            float px = 1. * view->geometry.origin.x / old_w;
            float py = 1. * view->geometry.origin.y / old_h;
            float pw = 1. * view->geometry.size.w / old_w;
            float ph = 1. * view->geometry.size.h / old_h;

            view->set_geometry(px * w, py * h, pw * w, ph * h);
        }
    });
}

wl_output_transform wayfire_output::get_transform()
{
    return transform;
}

std::tuple<int, int> wayfire_output::get_screen_size()
{
    return std::make_tuple(handle->width, handle->height);
}

void wayfire_output::ensure_pointer()
{
    auto ptr = weston_seat_get_pointer(core->get_current_seat());
    int px = wl_fixed_to_int(ptr->x), py = wl_fixed_to_int(ptr->y);

    auto g = get_full_geometry();
    if (!point_inside({px, py}, g)) {
        wl_fixed_t cx = wl_fixed_from_int(g.origin.x + g.size.w / 2);
        wl_fixed_t cy = wl_fixed_from_int(g.origin.y + g.size.h / 2);

        weston_pointer_motion_event ev;
        ev.mask |= WESTON_POINTER_MOTION_ABS;
        ev.x = wl_fixed_to_double(cx);
        ev.y = wl_fixed_to_double(cy);

        weston_pointer_move(ptr, &ev);
    }
}

void wayfire_output::activate()
{
}

void wayfire_output::deactivate()
{
    // TODO: what do we do?
}

void wayfire_output::attach_view(wayfire_view v)
{
    v->output = this;

    workspace->view_bring_to_front(v);

    auto sig_data = create_view_signal{v};
    signal->emit_signal("attach-view", &sig_data);
}

void wayfire_output::detach_view(wayfire_view v)
{
    auto sig_data = destroy_view_signal{v};
    signal->emit_signal("destroy-view", &sig_data);

    if (v->keep_count <= 0)
        workspace->view_removed(v);

    wayfire_view next = nullptr;

    auto views = workspace->get_views_on_workspace(workspace->get_current_workspace());
    for (auto wview : views) {
        if (wview->handle != v->handle && wview->is_mapped) {
            next = wview;
            break;
        }
    }

    if (active_view == v) {
        if (next == nullptr) {
            active_view = nullptr;
        } else {
            if (v->keep_count) {
                set_active_view(next);
            } else { /* Some plugins wants to keep the view, let it manage the position */
                focus_view(next, core->get_current_seat());
            }
        }
    }
}

void wayfire_output::bring_to_front(wayfire_view v) {
    assert(v);

    weston_view_geometry_dirty(v->handle);
    weston_layer_entry_remove(&v->handle->layer_link);

    workspace->view_bring_to_front(v);

    weston_view_geometry_dirty(v->handle);
    weston_surface_damage(v->surface);
    weston_desktop_surface_propagate_layer(v->desktop_surface);
}

void wayfire_output::set_active_view(wayfire_view v)
{
    if (v == active_view)
        return;

    if (active_view && !active_view->destroyed)
        weston_desktop_surface_set_activated(active_view->desktop_surface, false);

    active_view = v;
    if (v) {
        weston_view_activate(v->handle, core->get_current_seat(),
                WESTON_ACTIVATE_FLAG_CLICKED | WESTON_ACTIVATE_FLAG_CONFIGURE);
        weston_desktop_surface_set_activated(v->desktop_surface, true);
    }
}

void wayfire_output::focus_view(wayfire_view v, weston_seat *seat)
{
    set_active_view(v);

    if (v) {
        debug << "output: " << handle->id << " focus: " << v->desktop_surface << std::endl;
        bring_to_front(v);
    } else {
        debug << "output: " << handle->id << " focus: 0" << std::endl;
        weston_keyboard_set_focus(weston_seat_get_keyboard(seat), NULL);
    }
}

wayfire_view wayfire_output::get_top_view()
{
    if (active_view)
        return active_view;

    wayfire_view view;
    workspace->for_each_view([&view] (wayfire_view v) {
        if (!view)
            view = v;
    });

    return view;
}

wayfire_view wayfire_output::get_view_at_point(int x, int y)
{
    wayfire_view chosen = nullptr;

    workspace->for_each_view([x, y, &chosen] (wayfire_view v) {
        if (v->is_visible() && point_inside({x, y}, v->geometry)) {
            if (chosen == nullptr)
                chosen = v;
        }
    });

    return chosen;
}

bool wayfire_output::activate_plugin(wayfire_grab_interface owner)
{
    if (!owner)
        return false;

    if (core->get_active_output() != this)
        return false;

    if (active_plugins.find(owner) != active_plugins.end())
        return true;

    for(auto act_owner : active_plugins) {
        bool owner_in_act_owner_compat =
            act_owner->compat.find(owner->name) != act_owner->compat.end();

        bool act_owner_in_owner_compat =
            owner->compat.find(act_owner->name) != owner->compat.end();

        if(!owner_in_act_owner_compat && !act_owner->compatAll)
            return false;

        if(!act_owner_in_owner_compat && !owner->compatAll)
            return false;
    }

    active_plugins.insert(owner);
    return true;
}

bool wayfire_output::deactivate_plugin(wayfire_grab_interface owner)
{
    owner->ungrab();
    active_plugins.erase(owner);
    return true;
}

bool wayfire_output::is_plugin_active(owner_t name)
{
    for (auto act : active_plugins)
        if (act && act->name == name)
            return true;

    return false;
}
