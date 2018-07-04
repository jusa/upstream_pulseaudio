/***
  This file is part of PulseAudio.

  Copyright 2009 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/xmalloc.h>
#include <pulse/volume.h>

#include <pulsecore/macro.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/hook-list.h>
#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/modargs.h>
#include <pulsecore/llist.h>
#include <pulsecore/idxset.h>

#include "stream-interaction.h"

#define STREAM_INTERACTION_DEFAULT_DUCK_VOLUME_DB   (-20)
#define STREAM_INTERACTION_ANY_ROLE                 "any_role"
#define STREAM_INTERACTION_NO_ROLE                  "no_role"

struct group {
    char *name;
    pa_hashmap *trigger_roles;
    pa_hashmap *interaction_roles;
    pa_hashmap *trigger_state;
    pa_hashmap *interaction_state;
    pa_volume_t volume;
    bool any_role;
    bool active;

    PA_LLIST_FIELDS(struct group);
};

struct userdata {
    pa_core *core;
    PA_LLIST_HEAD(struct group, groups);
    bool global:1;
    bool duck:1;
};

static const char *sink_input_role(pa_sink_input *sink_input) {
    const char *role;

    pa_assert(sink_input);

    if (!(role = pa_proplist_gets(sink_input->proplist, PA_PROP_MEDIA_ROLE)))
        role = STREAM_INTERACTION_NO_ROLE;

    return role;
}

/* Return true if group state changes between
 * active and inactive. */
static bool update_group_active(struct userdata *u, struct group *g) {
    void *state = NULL;
    bool new_active = false;
    pa_sink_input *sink_input = NULL;
    void *value;

    pa_assert(u);
    pa_assert(g);

    if (pa_hashmap_size(g->trigger_state) > 0) {
        PA_HASHMAP_FOREACH_KV(sink_input, value, g->trigger_state, state) {
            if (!sink_input->muted &&
                sink_input->state != PA_SINK_INPUT_CORKED) {
                new_active = true;
                break;
            }
        }
    }

    if (new_active != g->active) {
        pa_log_debug("Group '%s' is now %sactive.", g->name, new_active ? "" : "in");
        g->active = new_active;
        return true;
    }

    return false;
}

/* Identify trigger streams and add or remove the streams from
 * state hashmap. Proplist change when changing media.role may
 * result in already existing stream to gain or lose trigger role
 * status. Returns true if the handled sink-input should be ignored
 * in interaction applying phase. */
static bool update_trigger_streams(struct userdata *u, struct group *g,
                                   pa_sink_input *sink_input, bool put, bool unlink,
                                   bool proplist) {
    const char *role = NULL;
    bool proplist_changed = false;
    bool can_ignore = false;

    pa_assert(u);
    pa_assert(g);
    pa_assert(sink_input);

    if (proplist) {
        bool in_trigger_state;
        bool in_trigger_roles;
        role = sink_input_role(sink_input);

        in_trigger_state = !!pa_hashmap_get(g->trigger_state, sink_input);
        in_trigger_roles = !!pa_hashmap_get(g->trigger_roles, role);

        /* If the sink-input is already both pointer in trigger_state hashmap
         * and role in trigger_roles, or neither, proplist value important to
         * us (media.role) didn't change, so no need to update anything. */
        proplist_changed = ((in_trigger_state && in_trigger_roles) ||
                            (!in_trigger_state && !in_trigger_roles));
    }

    if (proplist_changed || unlink) {
        if (pa_hashmap_remove(g->trigger_state, sink_input)) {
            can_ignore = true;
            pa_log_debug("Stream with role '%s' removed from group '%s'", sink_input_role(sink_input), g->name);
        }
    }

    if (proplist_changed || put) {
        if (!role)
            role = sink_input_role(sink_input);

        if (pa_hashmap_get(g->trigger_roles, role)) {
            pa_hashmap_put(g->trigger_state, sink_input, PA_INT_TO_PTR(1));
            can_ignore = true;
            pa_log_debug("Stream with role '%s' added to group '%s'", role, g->name);
        }
    }

    /* If proplist changed we need to include this stream into consideration
     * when applying interaction roles, as the sink-input may have gained or
     * lost trigger or interaction role. */
    if (proplist_changed)
        can_ignore = false;

    return can_ignore;
}

static void cork_or_duck(struct userdata *u, struct group *g,
                         pa_sink_input *sink_input, const char *interaction_role) {

    if (u->duck) {
        pa_cvolume vol;
        vol.channels = 1;
        vol.values[0] = g->volume;

        pa_log_debug("Group '%s' ducks a '%s' stream.", g->name, interaction_role);
        pa_sink_input_add_volume_factor(sink_input, g->name, &vol);

    } else {
        pa_log_debug("Group '%s' corks/mutes a '%s' stream.", g->name, interaction_role);
        if (!sink_input->muted)
            pa_sink_input_set_mute(sink_input, true, false);
        if (sink_input->state != PA_SINK_INPUT_CORKED)
            pa_sink_input_send_event(sink_input, PA_STREAM_EVENT_REQUEST_CORK, NULL);
    }
}

static void uncork_or_unduck(struct userdata *u, struct group *g,
                             pa_sink_input *sink_input, const char *interaction_role) {

    if (u->duck) {
        pa_log_debug("In '%s', found a '%s' stream that should be unducked", g->name, interaction_role);
        pa_sink_input_remove_volume_factor(sink_input, g->name);
    } else {
        pa_log_debug("In '%s' found a '%s' stream that should be uncorked/unmuted.", g->name, interaction_role);
        if (sink_input->muted)
           pa_sink_input_set_mute(sink_input, false, false);
        if (sink_input->state == PA_SINK_INPUT_CORKED)
            pa_sink_input_send_event(sink_input, PA_STREAM_EVENT_REQUEST_UNCORK, NULL);
    }
}

static void apply_interaction_to_sink_input(struct userdata *u, struct group *g,
                                            pa_sink_input *sink_input, bool unlink) {
    bool trigger = false;
    bool interaction_applied;
    const char *role = STREAM_INTERACTION_ANY_ROLE;

    pa_assert(u);

    if (pa_hashmap_get(g->trigger_state, sink_input))
        return;

    if (!g->any_role) {
        role = sink_input_role(sink_input);
        trigger = !!pa_hashmap_get(g->interaction_roles, role);
    }

    if (!g->any_role && !trigger)
        return;

    interaction_applied = !!pa_hashmap_get(g->interaction_state, sink_input);

    if (g->active && !interaction_applied) {
        pa_hashmap_put(g->interaction_state, sink_input, PA_INT_TO_PTR(1));
        cork_or_duck(u, g, sink_input, role);
    } else if ((unlink || !g->active) && interaction_applied) {
        pa_hashmap_remove(g->interaction_state, sink_input);
        uncork_or_unduck(u, g, sink_input, role);
    }
}

static void update_interactions(struct userdata *u, struct group *g,
                                pa_sink_input *sink_input, pa_sink_input *ignore,
                                bool unlink) {
    pa_sink_input *si;
    pa_idxset *idxset;
    uint32_t idx;

    pa_assert(u);
    pa_assert(g);
    pa_assert(sink_input);
    pa_assert(sink_input->sink);

    if (u->global)
        idxset = u->core->sink_inputs;
    else
        idxset = sink_input->sink->inputs;

    PA_IDXSET_FOREACH(si, idxset, idx) {
        if (si == ignore)
            continue;
        apply_interaction_to_sink_input(u, g, si, unlink);
    }
}

static void remove_interactions(struct userdata *u, struct group *g) {
    pa_sink_input *sink_input;
    void *value;
    void *state = NULL;

    pa_assert(u);
    pa_assert(g);

    PA_HASHMAP_FOREACH_KV(sink_input, value, g->interaction_state, state)
        uncork_or_unduck(u, g, sink_input, sink_input_role(sink_input));

    pa_hashmap_remove_all(g->trigger_state);
    pa_hashmap_remove_all(g->interaction_state);
}

static pa_hook_result_t process(struct userdata *u, pa_sink_input *sink_input,
                                bool put, bool unlink, bool proplist) {
    struct group *g;
    bool ignore = false;

    pa_assert(u);
    pa_sink_input_assert_ref(sink_input);

    if (!sink_input->sink)
        return PA_HOOK_OK;

    PA_LLIST_FOREACH(g, u->groups) {
        if (put || unlink || proplist)
            ignore = update_trigger_streams(u, g, sink_input, put, unlink, proplist);

        /* Update interactions when group state changes, or if there is new
         * stream added or removed while the group is already active. */
        if (update_group_active(u, g) || ((put || unlink) && g->active))
            update_interactions(u, g, sink_input, ignore ? sink_input : NULL, unlink);
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_put_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    return process(u, i, true, false, false);
}

static pa_hook_result_t sink_input_unlink_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_sink_input_assert_ref(i);

    return process(u, i, false, true, false);
}

static pa_hook_result_t sink_input_move_start_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    return process(u, i, false, true, false);
}

static pa_hook_result_t sink_input_move_finish_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    return process(u, i, true, false, false);
}

static pa_hook_result_t sink_input_state_changed_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    if (PA_SINK_INPUT_IS_LINKED(i->state))
        process(u, i, false, false, false);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_mute_changed_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    if (PA_SINK_INPUT_IS_LINKED(i->state))
        process(u, i, false, false, false);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_proplist_changed_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    if (PA_SINK_INPUT_IS_LINKED(i->state))
        process(u, i, false, false, true);

    return PA_HOOK_OK;
}

static struct group *group_new(const char *prefix, uint32_t id) {
    struct group *g = pa_xnew0(struct group, 1);
    PA_LLIST_INIT(struct group, g);
    g->trigger_roles = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func,
                                           pa_xfree, NULL);
    g->interaction_roles = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func,
                                               pa_xfree, NULL);
    g->trigger_state = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
    g->interaction_state = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
    g->volume = pa_sw_volume_from_dB(STREAM_INTERACTION_DEFAULT_DUCK_VOLUME_DB);
    g->name = pa_sprintf_malloc("%s_group_%u", prefix, id);

    return g;
}

static void group_free(struct group *g) {
    pa_hashmap_free(g->trigger_roles);
    pa_hashmap_free(g->interaction_roles);
    pa_hashmap_free(g->trigger_state);
    pa_hashmap_free(g->interaction_state);
    pa_xfree(g->name);
    pa_xfree(g);
}

typedef bool (*group_value_add_t)(struct group *g, const char *data);

static bool group_value_add_trigger_roles(struct group *g, const char *data) {
    pa_hashmap_put(g->trigger_roles, pa_xstrdup(data), PA_INT_TO_PTR(1));
    return true;
}

static bool group_value_add_interaction_roles(struct group *g, const char *data) {
    pa_hashmap_put(g->interaction_roles, pa_xstrdup(data), PA_INT_TO_PTR(1));
    if (pa_streq(data, STREAM_INTERACTION_ANY_ROLE))
        g->any_role = true;
    return true;
}

static bool group_value_add_volume(struct group *g, const char *data) {
    if (pa_parse_volume(data, &(g->volume)) < 0) {
        pa_log("Failed to parse volume");
        return false;
    }
    return true;
}

static bool group_parse_roles(pa_modargs *ma, const char *name, group_value_add_t add_cb, struct group *groups) {
    const char *roles;
    char *roles_in_group;
    struct group *g;

    pa_assert(ma);
    pa_assert(name);
    pa_assert(add_cb);
    pa_assert(groups);

    g = groups;
    roles = pa_modargs_get_value(ma, name, NULL);

    if (roles) {
        const char *group_split_state = NULL;

        while ((roles_in_group = pa_split(roles, "/", &group_split_state))) {
            pa_assert(g);

            if (roles_in_group[0] != '\0') {
                const char *split_state = NULL;
                char *n = NULL;
                while ((n = pa_split(roles_in_group, ",", &split_state))) {
                    bool ret = true;

                    if (n[0] != '\0')
                        ret = add_cb(g, n);
                    else {
                        ret = false;
                        pa_log("Empty %s", name);
                    }

                    pa_xfree(n);
                    if (!ret)
                        goto fail;
                }
            } else {
                pa_log("Empty %s", name);
                goto fail;
            }

            g = g->next;
            pa_xfree(roles_in_group);
        }
    }

    return true;

fail:
    return false;
}

static int count_groups(pa_modargs *ma, const char *module_argument) {
    const char *val;
    int count = 0;

    pa_assert(ma);
    pa_assert(module_argument);

    val = pa_modargs_get_value(ma, module_argument, NULL);
    if (val) {
        const char *split_state = NULL;
        int len = 0;
        /* Count empty ones as well, empty groups will fail later
         * when parsing the groups. */
        while (pa_split_in_place(val, "/", &len, &split_state))
            count++;
    }

    return count;
}

int pa_stream_interaction_init(pa_module *m, const char* const v_modargs[]) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    bool global = false;
    uint32_t i = 0;
    uint32_t group_count_tr = 0;
    struct group *last = NULL;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, v_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);

    u->core = m->core;
    u->duck = pa_streq(m->name, "module-role-ducking");
    PA_LLIST_HEAD_INIT(struct group, u->groups);

    group_count_tr = count_groups(ma, "trigger_roles");

    if (u->duck) {
        uint32_t group_count_du = 0;
        uint32_t group_count_vol = 0;
        group_count_du = count_groups(ma, "ducking_roles");
        group_count_vol = count_groups(ma, "volume");

        if ((group_count_tr > 1 || group_count_du > 1 || group_count_vol > 1) &&
            ((group_count_tr != group_count_du) || (group_count_tr != group_count_vol))) {
            pa_log("Invalid number of groups");
            goto fail;
        }
    } else {
        uint32_t group_count_co;
        group_count_co = count_groups(ma, "cork_roles");

        if ((group_count_tr > 1 || group_count_co > 1) &&
            (group_count_tr != group_count_co)) {
            pa_log("Invalid number of groups");
            goto fail;
        }
    }

    /* create at least one group */
    if (group_count_tr == 0)
        group_count_tr = 1;

    for (i = 0; i < group_count_tr; i++) {
        struct group *g = group_new(u->duck ? "ducking" : "cork", i);
        PA_LLIST_INSERT_AFTER(struct group, u->groups, last, g);
        last = g;
    }

    if (!group_parse_roles(ma, "trigger_roles", group_value_add_trigger_roles, u->groups))
        goto fail;

    if (pa_hashmap_isempty(u->groups->trigger_roles)) {
        pa_log_debug("Using role 'phone' as trigger role.");
        pa_hashmap_put(u->groups->trigger_roles, pa_xstrdup("phone"), PA_INT_TO_PTR(1));
    }

    if (!group_parse_roles(ma,
                           u->duck ? "ducking_roles" : "cork_roles",
                           group_value_add_interaction_roles,
                           u->groups))
        goto fail;

    if (pa_hashmap_isempty(u->groups->interaction_roles)) {
        pa_log_debug("Using roles 'music' and 'video' as %s_roles.", u->duck ? "ducking" : "cork");
        pa_hashmap_put(u->groups->interaction_roles, pa_xstrdup("music"), PA_INT_TO_PTR(1));
        pa_hashmap_put(u->groups->interaction_roles, pa_xstrdup("video"), PA_INT_TO_PTR(1));
    }

    if (u->duck)
        if (!group_parse_roles(ma, "volume", group_value_add_volume, u->groups))
            goto fail;

    if (pa_modargs_get_value_boolean(ma, "global", &global) < 0) {
        pa_log("Invalid boolean parameter: global");
        goto fail;
    }
    u->global = global;

    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_INPUT_PUT], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_put_cb, u);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_unlink_cb, u);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_INPUT_STATE_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_state_changed_cb, u);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MUTE_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_mute_changed_cb, u);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_INPUT_PROPLIST_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_proplist_changed_cb, u);

    /* When global interaction is enabled we don't need to know which sink our sink-inputs
     * are connected to. */
    if (!u->global) {
        pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_START], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_move_start_cb, u);
        pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_FINISH], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_move_finish_cb, u);
    }

    pa_modargs_free(ma);

    return 0;

fail:
    pa_stream_interaction_done(m);

    if (ma)
        pa_modargs_free(ma);

    return -1;
}

void pa_stream_interaction_done(pa_module *m) {
    struct userdata* u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    while (u->groups) {
        struct group *g = u->groups;
        PA_LLIST_REMOVE(struct group, u->groups, g);
        remove_interactions(u, g);
        group_free(g);
    }

    pa_xfree(u);
}
