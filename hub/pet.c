/* The desk pet engine (docs/PETS.md): one pet from pets/<name>/pet.conf
 * that lives on the desk's machines.  It wanders between places (machines
 * whose agent can show the buddy window) by weight, minds its manners
 * (leaves a machine someone types on, does not arrive where someone is
 * typing, stays a while when called), travels (looks where it is going,
 * the destination says "on my way"), and looks about: glances, watches the
 * pointer across monitors, and drops everything for a wiggled mouse.
 *
 * A brain (anything on the panel port) can drive it: pet <name> where |
 * go <place> | say <text> | look <dx> <dy>, and hears pet <name> arrived |
 * clicked | summoned | left <place>. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "hub.h"

#define LOOK_AWAY_MS   900    /* the place it leaves looks where it is going */
#define ATTENTION_MS   3000   /* a wiggle or a click holds its eyes this long */
#define GLANCE_MS      1500
#define MOOD_MS        15000  /* how often it decides whether to watch the pointer */
#define BRAIN_LOOK_MS  3000   /* a brain's "look" holds this long */
#define RETRY_MS       2000   /* nowhere to go: try again */
#define NWORDS         5

enum { P_NOWHERE, P_LEAVING, P_TRAVEL, P_HERE };
enum { W_ARRIVE, W_LEAVE, W_CLICK, W_SUMMON, W_COMING };
static const char *word_keys[NWORDS] = { "arrive", "leave", "click", "summoned", "coming" };

static struct {
    int on;
    char id[32];                 /* its folder: what panel commands call it */
    char name[32];               /* what its window says */
    rkm_look look;
    /* behaviour, from pet.conf */
    double every_min;            /* minutes between moves, about; 0 = never wanders */
    double stay_min;             /* stays put this long after being called */
    double typing_s;             /* a key this recent means someone is typing */
    int leave_typing, avoid_typing;
    double trip_s;               /* how long the trip takes */
    double glance_s;             /* seconds between glances, about; 0 = never */
    int follow_pct;              /* % of the time it watches the pointer */
    double wiggle;               /* this much wiggle gets its attention; 0 = never */
    int weight[MAX_SCREENS];     /* per place; 0 = never wanders there */
    char words[NWORDS][256];     /* lines separated by | */
    char coming[128];            /* what the destination says during this trip */
    /* where it is */
    int state;                   /* P_* */
    Screen *at;                  /* P_HERE: here; P_LEAVING: leaving here */
    Screen *to;                  /* P_LEAVING, P_TRAVEL: going there */
    long long phase_end, next_wander, stay_until, arrived_ms;
    /* looking */
    int sent_mode, sent_gx, sent_gy;
    long long sent_ms, attention_until, glance_until, next_glance, mood_until, brain_until;
    int following, glance_gx, glance_gy;
} pet;

static double frand(double lo, double hi) { return lo + (hi - lo) * (rand() / (RAND_MAX + 1.0)); }

static char *trim(char *s)
{
    char *e;
    while (isspace((unsigned char)*s)) s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int truthy(const char *v)
{
    return !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcasecmp(v, "true") || !strcmp(v, "1");
}

static unsigned char byte_of(double v) { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5); }

static int rgb(const char *v, unsigned char *out)
{
    unsigned long c;
    char *end;
    if (*v == '#') v++;
    c = strtoul(v, &end, 16);
    if (end - v != 6) return -1;
    out[0] = (unsigned char)(c >> 16);
    out[1] = (unsigned char)(c >> 8);
    out[2] = (unsigned char)c;
    return 0;
}

static int look_key(const char *k, const char *v)
{
    rkm_look *l = &pet.look;
    double d = atof(v);
    if (!strcasecmp(k, "shape")) l->shape = byte_of(d * 10);           /* height : width */
    else if (!strcasecmp(k, "gap")) l->gap = byte_of(d * 100);         /* of an eye's half-width */
    else if (!strcasecmp(k, "pupil")) l->pupil = byte_of(d * 100);
    else if (!strcasecmp(k, "pupil_shape")) l->pupil_shape = byte_of(d * 10);
    else if (!strcasecmp(k, "rim")) l->rim = byte_of(d * 100);
    else if (!strcasecmp(k, "colors") || !strcasecmp(k, "colours"))
        l->flags = (unsigned char)(truthy(v) ? l->flags | RKM_LOOK_COLOR : l->flags & ~RKM_LOOK_COLOR);
    else if (!strcasecmp(k, "background")) {
        if (!strcasecmp(v, "plain")) l->flags |= RKM_LOOK_PLAIN;
        else if (!strcasecmp(v, "weave")) l->flags &= (unsigned char)~RKM_LOOK_PLAIN;
        else return -1;
    }
    else if (!strcasecmp(k, "lid")) l->lid = byte_of(d * 100);
    else if (!strcasecmp(k, "blink")) l->blink = byte_of(d);
    else if (!strcasecmp(k, "mouth")) l->mouth = byte_of(d * 100);
    else if (!strcasecmp(k, "smile")) l->smile = (signed char)(d < -1 ? -100 : d > 1 ? 100 : d * 100);
    else if (!strcasecmp(k, "paper")) return rgb(v, l->paper);
    else if (!strcasecmp(k, "eye")) return rgb(v, l->eye);
    else if (!strcasecmp(k, "ink")) return rgb(v, l->ink);
    else return -1;
    return 0;
}

/* pets/<id>/pet.conf: next to the config, then next to the hub's source
 * tree (hub/../pets), then /usr/local/share/retrokm/pets; or cfg.pets_dir */
static FILE *open_pack(const char *conf_path, char *path, size_t n)
{
    char dirs[3][540], exe[512];
    const char *slash = strrchr(conf_path, '/');
    ssize_t k;
    FILE *f = NULL;
    int i;

    if (cfg.pets_dir[0]) {
        snprintf(path, n, "%.400s/%.31s/pet.conf", cfg.pets_dir, cfg.pet);
        return fopen(path, "r");
    }
    snprintf(dirs[0], sizeof dirs[0], "%.*spets", slash ? (int)(slash - conf_path + 1) : 0, conf_path);
    k = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[k > 0 ? k : 0] = 0;
    if (k > 0 && strrchr(exe, '/')) *strrchr(exe, '/') = 0;
    snprintf(dirs[1], sizeof dirs[1], "%.511s/../pets", exe);
    snprintf(dirs[2], sizeof dirs[2], "/usr/local/share/retrokm/pets");
    for (i = 0; i < 3 && !f; i++) {
        snprintf(path, n, "%.500s/%.31s/pet.conf", dirs[i], cfg.pet);
        f = fopen(path, "r");
    }
    if (!f) snprintf(path, n, "%.500s/%.31s/pet.conf", dirs[0], cfg.pet);
    return f;
}

int pet_load(const char *conf_path)
{
    char path[600], line[512], sec[32] = "";
    int lineno = 0, i;
    FILE *f;

    memset(&pet, 0, sizeof pet);
    if (!cfg.pet[0] || !strcasecmp(cfg.pet, "none")) return 0;
    if (strchr(cfg.pet, '/') || cfg.pet[0] == '.') {
        fprintf(stderr, "pet = %s: just the folder's name, please\n", cfg.pet);
        return -1;
    }
    if (!(f = open_pack(conf_path, path, sizeof path))) {
        fprintf(stderr, "pet %s: no %s\n", cfg.pet, path);
        return -1;
    }
    snprintf(pet.id, sizeof pet.id, "%s", cfg.pet);
    snprintf(pet.name, sizeof pet.name, "%s", cfg.pet);
    rkm_look_default(&pet.look);
    pet.every_min = 20;
    pet.stay_min = 5;
    pet.typing_s = 10;
    pet.leave_typing = pet.avoid_typing = 1;
    pet.trip_s = 3;
    pet.glance_s = 20;
    pet.follow_pct = 30;
    pet.wiggle = 4;
    for (i = 0; i < MAX_SCREENS; i++) pet.weight[i] = 1;
    snprintf(pet.words[W_COMING], sizeof pet.words[0], "on my way");

    while (fgets(line, sizeof line, f)) {
        char *t = trim(line), *eq, *c, *k, *v;
        int bad = 0;
        lineno++;
        if (!*t || *t == '#' || *t == ';') continue;
        for (c = t + 1; *c; c++)
            if ((*c == '#' || *c == ';') && isspace((unsigned char)c[-1])) { *c = 0; break; }
        t = trim(t);
        if (*t == '[') {
            if (!(c = strchr(t, ']'))) bad = 1;
            else { *c = 0; snprintf(sec, sizeof sec, "%s", trim(t + 1)); }
        } else if (!(eq = strchr(t, '='))) {
            bad = 1;
        } else {
            *eq = 0;
            k = trim(t);
            v = trim(eq + 1);
            if (!sec[0]) {
                if (!strcasecmp(k, "name")) snprintf(pet.name, sizeof pet.name, "%s", v);
                else bad = 1;
            } else if (!strcasecmp(sec, "look")) {
                bad = look_key(k, v) < 0;
            } else if (!strcasecmp(sec, "wander")) {
                if (!strcasecmp(k, "every")) pet.every_min = atof(v);
                else if (!strcasecmp(k, "stay")) pet.stay_min = atof(v);
                else bad = 1;
            } else if (!strcasecmp(sec, "places")) {
                Screen *s = screen_by_name(k);
                if (!strcmp(k, "*")) for (i = 0; i < MAX_SCREENS; i++) pet.weight[i] = atoi(v);
                else if (s) pet.weight[s - cfg.screens] = atoi(v);
                else logmsg("pet %s: no screen %s here (%s:%d), skipped", pet.id, k, path, lineno);
            } else if (!strcasecmp(sec, "manners")) {
                if (!strcasecmp(k, "leave_when_typing")) pet.leave_typing = truthy(v);
                else if (!strcasecmp(k, "avoid_typing")) pet.avoid_typing = truthy(v);
                else if (!strcasecmp(k, "typing")) pet.typing_s = atof(v);
                else bad = 1;
            } else if (!strcasecmp(sec, "travel")) {
                if (!strcasecmp(k, "time")) pet.trip_s = atof(v);
                else bad = 1;
            } else if (!strcasecmp(sec, "looking")) {
                if (!strcasecmp(k, "glance")) pet.glance_s = atof(v);
                else if (!strcasecmp(k, "follow")) pet.follow_pct = atoi(v);
                else if (!strcasecmp(k, "wiggle")) pet.wiggle = atof(v);
                else bad = 1;
            } else if (!strcasecmp(sec, "words")) {
                for (i = 0; i < NWORDS && strcasecmp(k, word_keys[i]); i++) {}
                if (i < NWORDS) snprintf(pet.words[i], sizeof pet.words[i], "%s", v);
                else bad = 1;
            } else {
                bad = 1;
            }
        }
        if (bad) {
            fprintf(stderr, "%s:%d: cannot parse this line\n", path, lineno);
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    snprintf(pet.look.name, sizeof pet.look.name, "%s", pet.name);
    pet.on = 1;
    pet.state = P_NOWHERE;
    pet.next_wander = now_ms() + 1000;      /* let the agents connect first */
    srand((unsigned)now_ms() ^ (unsigned)getpid());
    logmsg("pet %s (\"%s\") from %s", pet.id, pet.name, path);
    return 0;
}

/* ---- talking to places -------------------------------------------------- */

static int is_place(const Screen *s) { return s && s->buddy; }

/* UTF-8 from pet.conf or a brain, in the agent's own charset */
static unsigned conv(const Screen *s, const char *text, unsigned char *out, unsigned max)
{
    size_t n = 0;
    unsigned char *t = text_from_utf8((const unsigned char *)text, strlen(text), s->buddy->charset, RKM_EOL_LF, &n);
    if (!t) return 0;
    if (n > max) n = max;
    memcpy(out, t, n);
    free(t);
    return (unsigned)n;
}

static void send_look(Screen *s)
{
    unsigned char p[RKM_LOOK_LEN + 1 + RKM_NAME_MAX];
    rkm_look l = pet.look;
    unsigned char nm[RKM_NAME_MAX];
    unsigned n = conv(s, pet.name, nm, RKM_NAME_MAX);
    memcpy(l.name, nm, n);
    l.name[n] = 0;
    hub_send(s->buddy, RKM_BUDDY_LOOK, p, rkm_look_pack(p, &l));
}

static void show(Screen *s, int on, const char *label)
{
    unsigned char p[1 + 48];
    unsigned n;
    if (!is_place(s)) return;
    n = on ? conv(s, label, p + 1, 48) : 0;
    p[0] = on ? RKM_BUDDY_SHOW : RKM_BUDDY_HIDE;
    hub_send(s->buddy, RKM_BUDDY, p, 1 + n);
}

static void say_on(Screen *s, const char *text, int secs)
{
    unsigned char p[1 + 120];
    if (!is_place(s) || !*text) return;
    p[0] = (unsigned char)secs;
    hub_send(s->buddy, RKM_BUDDY_SAY, p, 1 + conv(s, text, p + 1, 120));
}

static void gaze_on(Screen *s, int mode, int gx, int gy, int hold10, int blink)
{
    unsigned char p[5];
    if (!is_place(s)) return;
    p[0] = (unsigned char)mode;
    p[1] = (unsigned char)(signed char)(gx < -100 ? -100 : gx > 100 ? 100 : gx);
    p[2] = (unsigned char)(signed char)(gy < -100 ? -100 : gy > 100 ? 100 : gy);
    p[3] = (unsigned char)hold10;
    p[4] = (unsigned char)blink;
    hub_send(s->buddy, RKM_BUDDY_GAZE, p, 5);
    pet.sent_mode = mode;
    pet.sent_gx = gx;
    pet.sent_gy = gy;
    pet.sent_ms = now_ms();
}

/* One of the lines in a words list (a | b | c), at random; "" if none */
static const char *word(int w, char *buf, size_t n)
{
    const char *s = pet.words[w], *e;
    int count = 1, pick;
    for (e = s; *e; e++) count += *e == '|';
    pick = rand() % count;
    while (pick-- > 0) s = strchr(s, '|') + 1;
    e = strchr(s, '|');
    snprintf(buf, n, "%.*s", (int)(e ? e - s : (long)strlen(s)), s);
    {
        char *t = trim(buf);
        memmove(buf, t, strlen(t) + 1);
    }
    return buf;
}

static void tell(const char *what, const Screen *s)
{
    char line[128];
    snprintf(line, sizeof line, "pet %s %s %s\n", pet.id, what, s ? s->name : "-");
    hub_tell_panels(line);
}

/* Which way b is from a on the desk, -100..100 each way (look down if unknown) */
static void direction(const Screen *a, const Screen *b, int *gx, int *gy)
{
    Monitor *ma = hub_monitor_of(a), *mb = hub_monitor_of(b);
    double dx, dy, k;
    *gx = 0;
    *gy = 90;
    if (!ma || !mb || ma == mb) return;
    dx = mb->col - ma->col;
    dy = mb->row - ma->row;
    k = sqrt(dx * dx + dy * dy);
    *gx = (int)(dx / k * 90);
    *gy = (int)(dy / k * 90);
}

/* ---- where it goes ------------------------------------------------------ */

static int typed_on(const Screen *s, long long now)
{
    return s->last_key_ms && now - s->last_key_ms < pet.typing_s * 1000;
}

/* A place to wander to, by weight; not `not`, nor where someone is typing */
static Screen *choose(const Screen *not, long long now)
{
    int i, total = 0, pick;
    for (i = 0; i < cfg.nscreens; i++) {
        Screen *s = &cfg.screens[i];
        if (s == not || !is_place(s) || pet.weight[i] <= 0) continue;
        if (pet.avoid_typing && typed_on(s, now)) continue;
        total += pet.weight[i];
    }
    if (!total) return NULL;
    pick = rand() % total;
    for (i = 0; i < cfg.nscreens; i++) {
        Screen *s = &cfg.screens[i];
        if (s == not || !is_place(s) || pet.weight[i] <= 0) continue;
        if (pet.avoid_typing && typed_on(s, now)) continue;
        if ((pick -= pet.weight[i]) < 0) return s;
    }
    return NULL;
}

static void schedule_wander(long long now)
{
    pet.next_wander = pet.every_min > 0 ? now + (long long)(pet.every_min * 60000 * frand(0.5, 1.5)) : 0;
}

static void arrive(Screen *s, long long now)
{
    char w[128];
    pet.state = P_HERE;
    pet.at = s;
    pet.to = NULL;
    pet.arrived_ms = now;
    pet.sent_mode = -1;
    pet.attention_until = pet.glance_until = pet.brain_until = 0;
    pet.next_glance = pet.glance_s > 0 ? now + (long long)(pet.glance_s * 1000 * frand(0.5, 1.5)) : 0;
    show(s, 1, pet.name);
    say_on(s, word(W_ARRIVE, w, sizeof w), 4);
    if (!pet.next_wander || pet.next_wander < now) schedule_wander(now);
    logmsg("pet %s: on %s", pet.id, s->name);
    tell("arrived", s);
}

/* The window currently on a machine: where it is, leaves from, or is coming to */
static Screen *shown_at(void)
{
    switch (pet.state) {
    case P_HERE: case P_LEAVING: return pet.at;
    case P_TRAVEL: return pet.to;
    default: return NULL;
    }
}

/* Off to `to`: the place it is on looks that way, says goodbye, and goes */
static void travel(Screen *to, long long now)
{
    Screen *from = shown_at();
    char w[128];
    int gx, gy;
    if (!is_place(to)) return;
    if (pet.state == P_HERE && from == to) return;
    if (pet.state == P_TRAVEL && from == to) return;
    if (!is_place(from)) { arrive(to, now); return; }
    if (from == to) {                          /* it was only leaving: stay after all */
        show(to, 1, pet.name);
        pet.state = P_HERE;
        pet.to = NULL;
        return;
    }
    direction(from, to, &gx, &gy);
    gaze_on(from, RKM_GAZE_AT, gx, gy, (LOOK_AWAY_MS + 1500) / 100, 0);
    say_on(from, word(W_LEAVE, w, sizeof w), 2);
    pet.state = P_LEAVING;
    pet.at = from;
    pet.to = to;
    pet.phase_end = now + LOOK_AWAY_MS;
    logmsg("pet %s: %s -> %s", pet.id, from->name, to->name);
}

/* ---- looking ------------------------------------------------------------ */

static void look_about(long long now)
{
    Screen *s = pet.at, *ps;
    Monitor *mp = NULL, *ma;
    double fx, fy;
    int mode = RKM_GAZE_FREE, gx = 0, gy = 0;

    if (now < pet.brain_until) return;         /* a brain said where to look */
    if (pet.wiggle > 0 && hub_wiggle() >= pet.wiggle) pet.attention_until = now + ATTENTION_MS;
    if (now >= pet.mood_until) {
        pet.following = rand() % 100 < pet.follow_pct;
        pet.mood_until = now + MOOD_MS;
    }
    if (pet.next_glance && now >= pet.next_glance) {
        pet.glance_until = now + GLANCE_MS;
        pet.glance_gx = (int)frand(-90, 90);
        pet.glance_gy = (int)frand(-70, 70);
        pet.next_glance = now + (long long)(pet.glance_s * 1000 * frand(0.5, 1.5));
    }
    ps = hub_pointer(&mp, &fx, &fy);
    if ((now < pet.attention_until || pet.following) && ps) {
        if (ps == s) mode = RKM_GAZE_POINTER;
        else if ((ma = hub_monitor_of(s)) && mp && ma != mp) {
            /* towards the pointer on its monitor, from the middle of ours */
            double dx = mp->col + fx - (ma->col + 0.5), dy = mp->row + fy - (ma->row + 0.5);
            double k = fabs(dx) > fabs(dy) ? fabs(dx) : fabs(dy);
            mode = RKM_GAZE_AT;
            gx = (int)(dx / (k > 1e-6 ? k : 1) * 100);
            gy = (int)(dy / (k > 1e-6 ? k : 1) * 100);
        }
    } else if (now < pet.glance_until) {
        mode = RKM_GAZE_AT;
        gx = pet.glance_gx;
        gy = pet.glance_gy;
    }
    if (mode == pet.sent_mode && abs(gx - pet.sent_gx) < 6 && abs(gy - pet.sent_gy) < 6) return;
    if (now - pet.sent_ms < 150 && pet.sent_mode >= 0) return;   /* not too often */
    gaze_on(s, mode, gx, gy, 0, 0);
}

/* ---- the engine --------------------------------------------------------- */

void pet_tick(void)
{
    long long now = now_ms();
    Screen *t;

    if (!pet.on) return;
    switch (pet.state) {
    case P_NOWHERE:
        if (now < pet.next_wander) break;
        if ((t = choose(NULL, now))) { pet.next_wander = 0; arrive(t, now); }
        else pet.next_wander = now + RETRY_MS;
        break;
    case P_LEAVING:                            /* looked; now off it goes */
        if (now < pet.phase_end) break;
        show(pet.at, 0, "");
        {
            int gx, gy;
            if (!*word(W_COMING, pet.coming, sizeof pet.coming))
                snprintf(pet.coming, sizeof pet.coming, "%s", pet.name);
            show(pet.to, 1, pet.coming);
            direction(pet.to, pet.at, &gx, &gy);   /* watching for it to come */
            gaze_on(pet.to, RKM_GAZE_AT, gx, gy, 0, 0);
        }
        pet.state = P_TRAVEL;
        pet.at = NULL;
        pet.phase_end = now + (long long)(pet.trip_s * 1000);
        break;
    case P_TRAVEL:
        if (now >= pet.phase_end) {
            gaze_on(pet.to, RKM_GAZE_FREE, 0, 0, 0, 1);
            arrive(pet.to, now);
        }
        break;
    case P_HERE:
        if (now >= pet.stay_until) {
            Screen *s = pet.at;
            /* typed on since it came (keys while it was told to stay don't count) */
            long long since = pet.arrived_ms > pet.stay_until ? pet.arrived_ms : pet.stay_until;
            if (pet.leave_typing && s->last_key_ms > since && typed_on(s, now)) {
                if ((t = choose(s, now))) { travel(t, now); break; }
            } else if (pet.next_wander && now >= pet.next_wander) {
                schedule_wander(now);
                if ((t = choose(s, now))) { travel(t, now); break; }
            }
        }
        look_about(now);
        break;
    }
}

/* A machine that can show the pet connected (or came back) */
void pet_joined(Screen *s)
{
    if (!pet.on || !is_place(s)) return;
    send_look(s);
    if (shown_at() == s) show(s, 1, pet.state == P_TRAVEL ? pet.coming : pet.name);
}

/* ... and went */
void pet_left(Screen *s)
{
    long long now = now_ms();
    if (!pet.on) return;
    if (pet.state == P_HERE && pet.at == s) {
        pet.state = P_NOWHERE;
        pet.at = NULL;
        pet.next_wander = now + RETRY_MS;
        tell("left", s);
    } else if (pet.state == P_LEAVING && pet.at == s) {
        pet.phase_end = now;                   /* straight on its way */
    } else if ((pet.state == P_TRAVEL || pet.state == P_LEAVING) && pet.to == s) {
        if (pet.state == P_LEAVING) {          /* stays where it was */
            show(pet.at, 1, pet.name);
            pet.state = P_HERE;
            pet.to = NULL;
        } else {
            pet.state = P_NOWHERE;
            pet.next_wander = now + RETRY_MS;
        }
    }
}

/* Someone clicked its window (RKM_BUDDY_CLICK) or called it from a machine */
void pet_buddy_event(Screen *s, int ev)
{
    long long now = now_ms();
    char w[128];
    if (!pet.on) return;
    if (ev == RKM_BUDDY_SUMMON) {
        tell("summoned", s);
        pet.stay_until = now + (long long)(pet.stay_min * 60000);
        if (shown_at() == s && pet.state != P_LEAVING) say_on(s, word(W_SUMMON, w, sizeof w), 3);
        else travel(s, now);
    } else if (shown_at() == s) {
        tell("clicked", s);
        say_on(s, word(W_CLICK, w, sizeof w), 3);
        if (pet.state == P_HERE) {
            pet.attention_until = now + ATTENTION_MS;
            gaze_on(s, RKM_GAZE_POINTER, 0, 0, 0, 1);
        }
    }
}

/* pet [<name> where | go <place> | say <text> | look <dx> <dy> | look pointer | look free] */
int pet_command(int argc, char **argv, const char *rest, Conn *r)
{
    long long now = now_ms();
    Screen *s;
    if (strcasecmp(argv[0], "pet")) return 0;
    if (!pet.on) { hub_reply(r, "error there is no pet (pet = in the config)\n"); return 1; }
    if (argc >= 2 && strcasecmp(argv[1], pet.id) && strcasecmp(argv[1], pet.name)) {
        hub_reply(r, "error no pet %s\n", argv[1]);
        return 1;
    }
    if (argc <= 2 || !strcasecmp(argv[2], "where")) {
        if (pet.state == P_HERE) hub_reply(r, "pet %s at %s\n", pet.id, pet.at->name);
        else if (pet.state == P_NOWHERE) hub_reply(r, "pet %s nowhere\n", pet.id);
        else hub_reply(r, "pet %s travelling %s %s\n", pet.id, pet.at ? pet.at->name : "-", pet.to->name);
    } else if (!strcasecmp(argv[2], "go") && argc == 4) {
        if (!is_place((s = screen_by_name(argv[3])))) hub_reply(r, "error %s cannot show the pet\n", argv[3]);
        else {
            pet.stay_until = now + (long long)(pet.stay_min * 60000);
            travel(s, now);
        }
    } else if (!strcasecmp(argv[2], "say") && argc >= 4) {
        if (!(s = shown_at())) hub_reply(r, "error the pet is nowhere\n");
        else say_on(s, rest, 5);
    } else if (!strcasecmp(argv[2], "look") && argc >= 4) {
        /* the engine leaves its eyes alone for a while (free: at once) */
        if (!(s = shown_at())) hub_reply(r, "error the pet is nowhere\n");
        else if (!strcasecmp(argv[3], "free")) {
            gaze_on(s, RKM_GAZE_FREE, 0, 0, 0, 0);
            pet.brain_until = 0;
        } else if (!strcasecmp(argv[3], "pointer")) {
            gaze_on(s, RKM_GAZE_POINTER, 0, 0, BRAIN_LOOK_MS / 100, 0);
            pet.brain_until = now + BRAIN_LOOK_MS;
        } else if (argc == 5) {
            gaze_on(s, RKM_GAZE_AT, (int)(atof(argv[3]) * 100), (int)(atof(argv[4]) * 100), BRAIN_LOOK_MS / 100, 0);
            pet.brain_until = now + BRAIN_LOOK_MS;
        } else {
            hub_reply(r, "error pet <name> look <dx> <dy> | pointer | free\n");
        }
    } else {
        hub_reply(r, "error pet <name> where | go <place> | say <text> | look <dx> <dy>\n");
    }
    return 1;
}
