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
    pa_idxset *trigger_roles;
    pa_idxset *interaction_roles;
    pa_hashmap *interaction_state;
    pa_volume_t volume;

    PA_LLIST_FIELDS(struct group);
};

struct userdata {
    pa_core *core;
    PA_LLIST_HEAD(struct group, groups);
    bool global:1;
    bool duck:1;
    pa_hook_slot
        *sink_input_put_slot,
        *sink_input_unlink_slot,
        *sink_input_move_start_slot,
        *sink_input_move_finish_slot,
        *sink_input_state_changed_slot,
        *sink_input_mute_changed_slot,
        *sink_input_proplist_changed_slot;
};

static const char *get_trigger_role(struct userdata *u, pa_sink_input *i, struct group *g) {
    const char *role, *trigger_role;
    uint32_t role_idx;

    if (!(role = pa_proplist_gets(i->proplist, PA_PROP_MEDIA_ROLE)))
        role = STREAM_INTERACTION_NO_ROLE;

    if (g == NULL) {
        /* get it from all groups */
        PA_LLIST_FOREACH(g, u->groups) {
            PA_IDXSET_FOREACH(trigger_role, g->trigger_roles, role_idx) {
                if (pa_streq(role, trigger_role))
                    return trigger_role;
            }
        }
    } else {
        PA_IDXSET_FOREACH(trigger_role, g->trigger_roles, role_idx) {
            if (pa_streq(role, trigger_role))
                return trigger_role;
        }
    }

    return NULL;
}

static const char *find_trigger_stream(struct userdata *u, pa_sink *s, pa_sink_input *ignore, struct group *g) {
    pa_sink_input *j;
    uint32_t idx;
    const char *trigger_role;

    pa_assert(u);
    pa_sink_assert_ref(s);

    for (j = PA_SINK_INPUT(pa_idxset_first(s->inputs, &idx)); j; j = PA_SINK_INPUT(pa_idxset_next(s->inputs, &idx))) {

        if (j == ignore)
            continue;

        trigger_role = get_trigger_role(u, j, g);
        if (trigger_role && !j->muted && j->state != PA_SINK_INPUT_CORKED)
            return trigger_role;
    }

    return NULL;
}

static const char *find_global_trigger_stream(struct userdata *u, pa_sink *s, pa_sink_input *ignore, struct group *g) {
    const char *trigger_role = NULL;

    pa_assert(u);

    if (u->global) {
        uint32_t idx;
        PA_IDXSET_FOREACH(s, u->core->sinks, idx)
            if ((trigger_role = find_trigger_stream(u, s, ignore, g)))
                break;
    } else
        trigger_role = find_trigger_stream(u, s, ignore, g);

    return trigger_role;
}

static void cork_or_duck(struct userdata *u, pa_sink_input *i, const char *interaction_role,  const char *trigger_role, bool interaction_applied, struct group *g) {

    if (u->duck && !interaction_applied) {
        pa_cvolume vol;
        vol.channels = 1;
        vol.values[0] = g->volume;

        pa_log_debug("Found a '%s' stream of '%s' that ducks a '%s' stream.", trigger_role, g->name, interaction_role);
        pa_sink_input_add_volume_factor(i, g->name, &vol);

    } else if (!u->duck) {
        pa_log_debug("Found a '%s' stream that corks/mutes a '%s' stream.", trigger_role, interaction_role);
        pa_sink_input_set_mute(i, true, false);
        pa_sink_input_send_event(i, PA_STREAM_EVENT_REQUEST_CORK, NULL);
    }
}

static void uncork_or_unduck(struct userdata *u, pa_sink_input *i, const char *interaction_role, bool corked, struct group *g) {

    if (u->duck) {
       pa_log_debug("In '%s', found a '%s' stream that should be unducked", g->name, interaction_role);
       pa_sink_input_remove_volume_factor(i, g->name);
    }
    else if (corked || i->muted) {
       pa_log_debug("Found a '%s' stream that should be uncorked/unmuted.", interaction_role);
       if (i->muted)
          pa_sink_input_set_mute(i, false, false);
       if (corked)
          pa_sink_input_send_event(i, PA_STREAM_EVENT_REQUEST_UNCORK, NULL);
    }
}

static inline void apply_interaction_to_sink(struct userdata *u, pa_sink *s, const char *new_trigger, pa_sink_input *ignore, bool new_stream, struct group *g) {
    pa_sink_input *j;
    uint32_t idx, role_idx;
    const char *interaction_role;
    bool trigger = false;

    pa_assert(u);
    pa_sink_assert_ref(s);

    for (j = PA_SINK_INPUT(pa_idxset_first(s->inputs, &idx)); j; j = PA_SINK_INPUT(pa_idxset_next(s->inputs, &idx))) {
        bool corked, interaction_applied;
        const char *role;

        if (j == ignore)
            continue;

        if (!(role = pa_proplist_gets(j->proplist, PA_PROP_MEDIA_ROLE)))
            role = STREAM_INTERACTION_NO_ROLE;

        PA_IDXSET_FOREACH(interaction_role, g->interaction_roles, role_idx) {
            if ((trigger = pa_streq(role, interaction_role)))
                break;
            if ((trigger = (pa_streq(interaction_role, STREAM_INTERACTION_ANY_ROLE) &&
                            !get_trigger_role(u, j, g))))
               break;
        }
        if (!trigger)
            continue;

        /* Some applications start their streams corked, so the stream is uncorked by */
        /* the application only after sink_input_put() was called. If a new stream turns */
        /* up, act as if it was not corked. In the case of module-role-cork this will */
        /* only mute the stream because corking is reverted later by the application */
        corked = (j->state == PA_SINK_INPUT_CORKED);
        if (new_stream && corked)
            corked = false;
        interaction_applied = !!pa_hashmap_get(g->interaction_state, j);

        if (new_trigger && ((!corked && !j->muted) || u->duck)) {
            if (!interaction_applied)
                pa_hashmap_put(g->interaction_state, j, PA_INT_TO_PTR(1));

            cork_or_duck(u, j, role, new_trigger, interaction_applied, g);

        } else if (!new_trigger && interaction_applied) {
            pa_hashmap_remove(g->interaction_state, j);

            uncork_or_unduck(u, j, role, corked, g);
        }
    }
}

static void apply_interaction(struct userdata *u, pa_sink *s, const char *trigger_role, pa_sink_input *ignore, bool new_stream, struct group *g) {
    pa_assert(u);

    if (u->global) {
        uint32_t idx;
        PA_IDXSET_FOREACH(s, u->core->sinks, idx)
            apply_interaction_to_sink(u, s, trigger_role, ignore, new_stream, g);
    } else
        apply_interaction_to_sink(u, s, trigger_role, ignore, new_stream, g);
}

static void remove_interactions(struct userdata *u, struct group *g) {
    uint32_t idx, idx_input;
    pa_sink *s;
    pa_sink_input *j;
    bool corked;
    const char *role;

    PA_IDXSET_FOREACH(s, u->core->sinks, idx) {

        for (j = PA_SINK_INPUT(pa_idxset_first(s->inputs, &idx_input)); j; j = PA_SINK_INPUT(pa_idxset_next(s->inputs, &idx_input))) {

            if(!!pa_hashmap_get(g->interaction_state, j)) {
                corked = (j->state == PA_SINK_INPUT_CORKED);
                if (!(role = pa_proplist_gets(j->proplist, PA_PROP_MEDIA_ROLE)))
                   role = STREAM_INTERACTION_NO_ROLE;
                uncork_or_unduck(u, j, role, corked, g);
            }
        }
    }
}

static pa_hook_result_t process(struct userdata *u, pa_sink_input *i, bool create, bool new_stream) {
    const char *trigger_role;
    struct group *g;

    pa_assert(u);
    pa_sink_input_assert_ref(i);

    if (!create)
        PA_LLIST_FOREACH(g, u->groups)
            pa_hashmap_remove(g->interaction_state, i);

    if (!i->sink)
        return PA_HOOK_OK;

    PA_LLIST_FOREACH(g, u->groups) {
        trigger_role = find_global_trigger_stream(u, i->sink, create ? NULL : i, g);
        apply_interaction(u, i->sink, trigger_role, create ? NULL : i, new_stream, g);
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_put_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    return process(u, i, true, true);
}

static pa_hook_result_t sink_input_unlink_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_sink_input_assert_ref(i);

    return process(u, i, false, false);
}

static pa_hook_result_t sink_input_move_start_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    return process(u, i, false, false);
}

static pa_hook_result_t sink_input_move_finish_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    return process(u, i, true, false);
}

static pa_hook_result_t sink_input_state_changed_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    if (PA_SINK_INPUT_IS_LINKED(i->state) && get_trigger_role(u, i, NULL))
        return process(u, i, true, false);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_mute_changed_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    if (PA_SINK_INPUT_IS_LINKED(i->state) && get_trigger_role(u, i, NULL))
        return process(u, i, true, false);

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_input_proplist_changed_cb(pa_core *core, pa_sink_input *i, struct userdata *u) {
    pa_core_assert_ref(core);
    pa_sink_input_assert_ref(i);

    if (PA_SINK_INPUT_IS_LINKED(i->state))
        return process(u, i, true, false);

    return PA_HOOK_OK;
}

static struct group *group_new(const char *prefix, uint32_t id) {
    struct group *g = pa_xnew0(struct group, 1);
    PA_LLIST_INIT(struct group, g);
    g->trigger_roles = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func,
                                           pa_xfree, NULL);
    g->interaction_roles = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func,
                                               pa_xfree, NULL);
    g->interaction_state = pa_hashmap_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
    g->volume = pa_sw_volume_from_dB(STREAM_INTERACTION_DEFAULT_DUCK_VOLUME_DB);
    g->name = pa_sprintf_malloc("%s_group_%u", prefix, id);

    return g;
}

static void group_free(struct group *g) {
    pa_idxset_free(g->trigger_roles, pa_xfree);
    pa_idxset_free(g->interaction_roles, pa_xfree);
    pa_hashmap_free(g->interaction_state);
    pa_xfree(g->name);
    pa_xfree(g);
}

typedef bool (*group_value_add_t)(struct group *g, const char *data);

static bool group_value_add_trigger_roles(struct group *g, const char *data) {
    pa_idxset_put(g->trigger_roles, pa_xstrdup(data), NULL);
    return true;
}

static bool group_value_add_interaction_roles(struct group *g, const char *data) {
    pa_idxset_put(g->interaction_roles, pa_xstrdup(data), NULL);
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

    if (pa_idxset_isempty(u->groups->trigger_roles)) {
        pa_log_debug("Using role 'phone' as trigger role.");
        pa_idxset_put(u->groups->trigger_roles, pa_xstrdup("phone"), NULL);
    }

    if (!group_parse_roles(ma,
                           u->duck ? "ducking_roles" : "cork_roles",
                           group_value_add_interaction_roles,
                           u->groups))
        goto fail;

    if (pa_idxset_isempty(u->groups->interaction_roles)) {
        pa_log_debug("Using roles 'music' and 'video' as %s_roles.", u->duck ? "ducking" : "cork");
        pa_idxset_put(u->groups->interaction_roles, pa_xstrdup("music"), NULL);
        pa_idxset_put(u->groups->interaction_roles, pa_xstrdup("video"), NULL);
    }

    if (u->duck)
        if (!group_parse_roles(ma, "volume", group_value_add_volume, u->groups))
            goto fail;

    if (pa_modargs_get_value_boolean(ma, "global", &global) < 0) {
        pa_log("Invalid boolean parameter: global");
        goto fail;
    }
    u->global = global;

    u->sink_input_put_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_PUT], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_put_cb, u);
    u->sink_input_unlink_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_unlink_cb, u);
    u->sink_input_move_start_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_START], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_move_start_cb, u);
    u->sink_input_move_finish_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_FINISH], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_move_finish_cb, u);
    u->sink_input_state_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_STATE_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_state_changed_cb, u);
    u->sink_input_mute_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_MUTE_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_mute_changed_cb, u);
    u->sink_input_proplist_changed_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_PROPLIST_CHANGED], PA_HOOK_LATE, (pa_hook_cb_t) sink_input_proplist_changed_cb, u);

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

    if (u->sink_input_put_slot)
        pa_hook_slot_free(u->sink_input_put_slot);
    if (u->sink_input_unlink_slot)
        pa_hook_slot_free(u->sink_input_unlink_slot);
    if (u->sink_input_move_start_slot)
        pa_hook_slot_free(u->sink_input_move_start_slot);
    if (u->sink_input_move_finish_slot)
        pa_hook_slot_free(u->sink_input_move_finish_slot);
    if (u->sink_input_state_changed_slot)
        pa_hook_slot_free(u->sink_input_state_changed_slot);
    if (u->sink_input_mute_changed_slot)
        pa_hook_slot_free(u->sink_input_mute_changed_slot);
    if (u->sink_input_proplist_changed_slot)
        pa_hook_slot_free(u->sink_input_proplist_changed_slot);

    pa_xfree(u);

}
