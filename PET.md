# The pet

A creature lives on the 3.5" gadget. Its life runs on the board, once a
second, and is saved to flash, so it goes on when the power is off (at half
pace) and after a reboot. Muse is its spirit: the board reports to Muse when
something happens and every so often, and Muse answers in the creature's
voice (a speech bubble under it) and can look after it with the `pet.*`
commands. The pet never needs Muse online to live.

## The screen

Top: the creature, drawn from its genome. Under its feet: its name, stage and
age; its mood and health; five bars (FOOD, REST, FUN, CLEAN, LOVE; full is
good); and the buttons:

- **FEED** a meal (+40 food). It poops about 40 minutes after eating.
- **PLAY** a ten-second game: tap the ball every time it jumps. Hits raise
  fun and love and cost a little rest.
- **CLEAN** bathes it and clears the floor. Poop on the floor lowers CLEAN
  and, left there, its health.
- **MEDS** when it is sick (two doses cure it). Lit when needed.
- **ZZZ / WAKE** lights out (it sleeps and regains rest) or back on. It
  also falls asleep by itself when worn out, and sleeps 23:00 to 07:00.

Tap the creature to pet it (+love; too many taps in a row and it has had
enough). Hold the BOOT button to talk to it: Muse answers as the creature,
in the bubble.

## Its life

- **Egg.** Tap it to warm it; twenty taps hatch it, or it hatches on its
  own after six hours. The egg's colours are the creature's.
- **Baby (first day), kid (to day 3), teen (to day 7), adult (to day 21),
  elder.** Babies are hungriest. Kids get feet, teens their head feature
  (ears, horns, antennae, crest or fin), adults their tail and markings.
- **Evolution.** At each stage's end its body changes with how it was
  cared for: well kept becomes *noble* (horns or a crest, brighter), middling
  becomes *cute* (ears, big eyes, spots), neglected becomes *feral* (fangs,
  spikes, stripes). Muse can choose with `pet.evolve`.
- **Needs** drain by the hour at rates set by its stage and traits (bold,
  lazy, sociable, greedy; one or two per creature). Below 40 for long and
  health stops recovering; below 15 and health falls; under 40 health it is
  **sick** (green, sweating) until it gets two doses of medicine.
- **Leaving.** A full day at zero health and it runs away, leaving a new egg
  of the next generation, coloured like its parent.

## Muse

The board sends Muse a short report when it hatches, evolves, gets hungry,
gets sick, wakes, or runs away, when a mood changes to a needy one (at most
every 30 minutes), and routinely every `CONFIG_MUSE_PET_REPORT_MIN` minutes
(90). The report describes the creature and its state and asks for one short
line in the creature's voice; that reply shows in the bubble. Muse can also
act:

| command | does |
| --- | --- |
| `pet.status` | everything about it, including a plain-words description |
| `pet.feed` (`snack`) | a meal, or a snack (fun, but unhealthy when full) |
| `pet.play` (`score` 0-10) | a game it played with Muse |
| `pet.clean`, `pet.medicine` | as the buttons |
| `pet.lights` (`off`) | lights out or on |
| `pet.name` (`name`) | names it (the hatch report asks Muse to) |
| `pet.say` (`text`, `seconds`) | the bubble, with a chirp |
| `pet.set_mood` (`mood`, `minutes`) | overrides how it feels and acts for a while |
| `pet.evolve` (`variant`) | noble, cute or feral; now if the stage is nearly over |
| `pet.new_egg` (`confirm`) | gives up on it and starts the next generation |
| `pet.time_scale` (`scale`) | 1 = real time, up to 200 (an hour every 18 s), for watching it grow; not saved |
| `pet.pet` | a stroke, as a tap |

Typed turns from the USB console reach the same Muse: `tools/muse/chat.py
"call pet.time_scale with scale 200"`.

## Code

- `esp32/components/muse/pet.c` the life: genome, needs, sleep, stages,
  evolution, sickness, persistence (NVS namespace `pet`), chirps (synthesised
  in its own pitch), reports and Muse's typed replies.
- `esp32/components/muse/avatar/muse_pixel.c` the creature renderer: body,
  features, face, animations (breathe, blink, hop, chomp, snore, sweat, cry,
  talk with Muse's speech), overlays and the egg, from the genome. The Bonzi
  sprite avatar is kept beside it as `muse_pixel.c.bonzi`.
- `esp32/main/gadget_pet.c` the `pet.*` Home Link commands.
- `esp32/components/muse/muse_ui.c` the panel, the game and the bubble
  (`CONFIG_MUSE_PET`).
