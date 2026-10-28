# Desk pets (design, 2026-10-28)

Status: design, not built yet.

A desk pet is a little character that lives on the desk's screens and
gadgets and moves between them: a face in a corner of whichever machine it
is visiting, on the touch panel, on a TV, on a robot's head. The owner's
own pet, lil' C, is private and stays in his own repository. This is the
public part: an engine in RetroKM that anyone can give their own pet to,
and a builder to make one.

## What a pet is

Three parts, kept apart so each can be shared or kept private on its own:

- **Pack**: how it looks and how it behaves. A folder `pets/<name>/` with
  `pet.conf` and any pictures. Made with the builder, or by hand.
- **Engine** (in the hub, public): where the pet is, when it wanders, how it
  travels, what it looks at. Same for every pet.
- **Brain** (optional, the pet's own): anything that gives it more to say
  or do: a voice, a Claude conversation, a robot body. It talks to the
  engine over the panel port, like lil' C's bridge does today. A pet with
  no brain still wanders, looks around and comes when called.

Packs and brains are files on the owner's machine. `pets/` is ignored by
git except the example pet, so nobody's pet ends up in the repository by
accident.

## Places

A **place** is somewhere a pet can be. It says what it can do when it
connects:

| place | how it connects | can |
|---|---|---|
| a machine's agent (X11, Windows, ...) | the BUDDY capability, as today | draw, be clicked, be called from the machine |
| the touch panel / phone page | panel port | draw, be tapped, summon |
| a TV, a desk display (Kodi, IA-1, Audrey) | panel port, a small client | draw; some: be tapped, play sound |
| a robot or gadget (a pet's own body) | its brain | whatever the brain does |

A place that can only show eyes gets eyes; one that can show pictures gets
the pack's pictures. Old machines (IRIX, classic Mac) keep working with the
eyes.

## How a pet looks

Two ways, and a pack can have both (pictures where they can be shown, eyes
everywhere else):

- **Eyes**: drawn by the place itself from a few numbers, so even tiny
  agents can do it: eye shape (width : height), spacing, pupil size and
  shape, colours (or black and white), lids and blink, background (plain,
  weave, colour), an optional mouth line. The pupils follow a gaze the
  engine sends. Today's buddy window is this with one fixed set of numbers.
- **Pictures**: small frames (up to 128x128, indexed colour or 1-bit),
  named by mood or action: `idle`, `blink`, `look-left`, `walk`, `sleep`,
  `happy`, `talk`. The hub sends a place each frame once, then just which
  one to show.

## How a pet behaves

The engine reads these from `pet.conf`; the builder shows them as sliders.

- **Wandering**: how often it moves (minutes), and a weight per place
  (more time on the TV, never on the work PC).
- **Manners**: leaves a machine when someone types on it; doesn't arrive
  where someone is typing; stays put for N minutes after being called.
- **Travel**: the place it leaves turns to look where it's going, then it
  slides off; the place it goes to shows "on my way" for the trip time.
- **Looking**: glances around now and then, sometimes watches the mouse
  across monitors, and drops everything for a wiggled mouse (the hub
  already measures the wiggle). How much of each is a slider.
- **Moods** (pictures only): which frames to use when idle, travelling,
  sleeping (no input for a while), happy (just called).
- **Words**: optional short lines for arriving, leaving, being clicked,
  shown as a caption. A brain can say anything instead.

## Protocol

Today: BUDDY (show / hide + label) to agents, BUDDY_EVENT (click /
summon) back, and `buddy <screen> show|hide` on the panel port. Added:

- BUDDY_LOOK (hub to place): the eye numbers, and pictures if it can show
  them, sent before the first show and when the pack changes.
- BUDDY_GAZE: where to look (-1..1 across and down, or "at the pointer"),
  blink, which picture.
- BUDDY_SAY: a caption.
- Panel port: `pet <name> where`, `pet <name> go <place>`, `pet <name> say
  <text>`, `pet <name> look <dx> <dy>`, and events `pet <name> arrived
  <place>` / `clicked` / `summoned` for a brain.

The agents keep working with an old hub, and an old agent shows the default
eyes, so nothing has to be updated at once. `docs/PROTOCOL.md` gets a
protocol version number with this.

## The builder

A static page (with the setup planner, no server): pick or draw eyes and
see them move, draw or import picture frames (pixel editor, with a
preview at real size on each kind of place), set behaviour with sliders,
write its words, then download the pack as a zip to unpack into `pets/`.
It can also open a pack to change it.

## lil' C

Nothing here changes lil' C. His bridge, bodies and look stay in his
private repository. Later he could become a private pack plus his bridge as
the brain on top of this engine, so the wandering code lives in one place,
but that is the owner's call.

What does change in public RetroKM: the buddy windows and docs talk about
"the desk pet" instead of lil' C by name, and his eye shape becomes the
default eyes only if his pack says so. The example pet in `pets/` is a new
character, so the default look belongs to RetroKM.

## Order of work

1. **Generic buddy window**: the X11 and Windows windows draw eyes from
   numbers (today's look as the numbers), with no names in the code;
   BUDDY_LOOK and BUDDY_GAZE.
2. **Engine in the hub**: places, wandering, manners, travel, looking; one
   pet from `pets/`; an example pet; tests with fake agents.
3. **Panel and phone page** show the pet and can call it.
4. **Pictures** for places that can show them.
5. **Builder** page.
6. **Small places**: the Kodi add-on, the IA-1 and the Audrey as panel
   clients.
7. Several pets at once (they could meet).

## Open questions

- The example pet: what it is called and what it looks like.
- Does the hub do the wandering for lil' C too, or does his bridge keep
  its own? (The owner's call; both work.)
- Sound: places that can play sound could play a pet's chirps or voice
  clips. Later.
