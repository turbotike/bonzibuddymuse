# The pet

A dinosaur lives on the 3.5" gadget, Digimon style. Its life runs on the
board, once a second, and is saved to flash, so it goes on when the power is
off (at half pace) and after a reboot. Muse is its spirit: the board reports
to Muse when something happens and every so often, and Muse answers in the
creature's voice (a cartoon speech bubble above it) and can look after it
with the `pet.*` commands. The pet never needs Muse online to live. Mat talks
to Muse in the app; the gadget itself shows no text beyond the bubble.

## The toy

The screen is a 90s virtual pet: a plastic shell in the theme's colour round
a neon-rimmed LCD, and three big round buttons under it.

- **The scene** fills the LCD: a mountain range with a smoking volcano,
  rolling hills and a grass field he paces about on, turning to face the
  way he goes and hopping now and then. The sky follows the clock: blue by
  day, peach at dawn and dusk, deep night with stars and a moon from 20:00
  to 06:00. Weather changes every twenty minutes or so: clear, overcast, or
  rain with a grey sky, low clouds, streaks and puddles.
- **The LCD** shows the creature, an icon bar along the top (FOOD, TRAIN,
  BATTLE, CLEAN, MEDS, LIGHT, STATS, THEME) and, along the foot on the ground,
  its name, stage, age, mood, HP and POWER. Little icons blink there when it
  is hungry, dirty, sick, lonely or bored.
- **Buttons:** the right arrow moves the highlight along the icons, the
  enter arrow does the highlighted thing (or opens the stats page when
  nothing is highlighted), the X cancels and closes pages and bubbles. Icons
  can also just be tapped.
- **Tap the creature** to pet it (+love; too many taps in a row and it has had
  enough). Tap an egg to warm it.
- **The bubble** appears at the top of the LCD for the creature's words (from
  Muse, or its own short remarks) and goes after a few seconds.
- **Themes:** NEON, TOXIC, LAVA and ARCADE, cycled with the THEME icon (or
  `pet.theme`), kept with the pet.
- Swiping left still reaches the Wi-Fi and pairing settings. Holding BOOT
  still sends a voice note to Muse; the answer comes back in the bubble.

## Care

- **FEED** a meal (+40 food). It poops about 40 minutes after eating.
- **TRAIN** is a button drill: a big arrow, enter or X flashes up on the LCD
  with a timer bar; press that button before it runs out and the dino fires
  at the boulder. Eight rounds, a little faster each. Hits raise FUN, LOVE
  and **POWER**; the boulder cracks as the hits add up. POWER fades slowly
  over days and decides, together with care, how it evolves.
- **CLEAN** bathes it and clears the floor. Poop on the floor lowers CLEAN
  and, left there, its health.
- **MEDS** when it is sick (two doses cure it).
- **LIGHT** lights out (it sleeps and regains REST) or back on. It also falls
  asleep by itself when worn out, and sleeps 23:00 to 07:00.
- **STATS** the six bars (FOOD, REST, FUN, CLEAN, LOVE, POWER), HP, care,
  generation, stage and age.

## Battles

Wild dinos turn up at random while it is awake (a rookie or older, not sick),
roughly every few hours, and the BATTLE icon picks a fight any time. Battles
are Pokemon-style turns. Your dino stands on the left, the enemy mirrored on
the right, with HP bars and a message line.

- **Stats** come from the genome, the stage and POWER: HP, attack (teeth and
  a bold nature help), defence (stego and ankylo armour), speed (raptor and
  ptero are quick). The faster one acts first.
- **Elements** by species: FIRE (rex), WIND (raptor, ptero), LEAF (long-neck,
  tri-horn), ROCK (stego, ankylo). Fire beats leaf, leaf beats rock, rock
  beats wind, wind beats fire: double damage one way, half the other.
- **Moves:** BITE (always hits), the species' special (FLAME BLAST, GUST, VINE
  WHIP, ROCK SMASH: stronger, 85% to hit, carries the element), GUARD (halves
  the next blow) and RUN (likelier when you are the faster). The right arrow
  moves along the menu, enter picks, X runs; the cells can be tapped too.
  Criticals happen one time in ten.
- **Winning** raises POWER (more against a stronger enemy), FUN and LOVE.
  **Losing** costs a little health and fun. A wild dino ignored for 45 seconds
  wanders off. Muse hears about wins and losses, and can start a wild
  encounter with `pet.battle`.

## Its life

- **Egg.** Twenty taps hatch it, or six hours. The shell is coloured like the
  creature inside.
- **Baby (first day):** an in-training blob with a face and a nub of tail.
- **Rookie (to day 3):** a chibi dino of its species. **Champion (to day 7):**
  its crest and back features come in, a horn crown, bigger teeth, a
  fighting stance. **Ultimate (to day 21):** full size, tail tip and
  markings, and digital armour: a metal helmet the horns pierce, a chest
  plate, a shoulder pad, a metal claw arm on two-legged species. **Mega:**
  bigger again, with wings, a snout mask, spiked pads and glowing eyes. The
  metal is gold on the noble path, dark steel on the feral path, silver
  otherwise.
- **Species:** rex, raptor, long-neck, stego, tri-horn, ankylo, ptero; each
  with its own body plan, and a genome for size, head, neck, tail, eyes, jaw,
  teeth, crest (nose horn, brow horns, feather crest, frill and horns, long
  head crest), back (spikes, plates, sail, armour bumps), tail tip (club,
  spikes, tuft), markings and colours. Every egg is a new one; a new egg from
  a parent usually keeps the species and the family colour.
- **Evolution.** At each stage's end its body changes: well cared for and
  well trained becomes *noble* (a grander crest and back, brighter, bigger),
  well cared for becomes *cute* (bigger eyes and head, a round snout, spots),
  neglected becomes *feral* (fangs, spikes, a spiked tail, stripes). Muse can
  choose with `pet.evolve`.
- **Needs** drain by the hour at rates set by stage and traits (bold, lazy,
  sociable, greedy). Under 40 health it is **sick** until medicined; a full
  day at zero health and it runs away, leaving a new egg of the next
  generation.

## Muse

The board sends Muse a short report when it hatches, evolves, gets hungry or
sick, wakes, or runs away, when a mood turns needy (at most every 30 minutes)
and routinely every `CONFIG_MUSE_PET_REPORT_MIN` minutes (90). The report
describes the creature and its state and asks for one short line in its
voice; that line is the bubble. Muse can also act:

| command | does |
| --- | --- |
| `pet.status` | everything about it, with a plain-words description |
| `pet.feed` (`snack`) | a meal, or a snack (fun, but unhealthy when full) |
| `pet.train` (`hits` 0-5) | a training session Muse ran with it |
| `pet.clean`, `pet.medicine` | as the icons |
| `pet.lights` (`off`) | lights out or on |
| `pet.name` (`name`) | names it (the hatch report asks Muse to) |
| `pet.say` (`text`, `seconds`) | the bubble, with a chirp |
| `pet.set_mood` (`mood`, `minutes`) | overrides how it feels and acts for a while |
| `pet.evolve` (`variant`) | noble, cute or feral; now if the stage is nearly over |
| `pet.theme` (`theme`) | the colour scheme, 0-3; omitted cycles |
| `pet.battle` | a wild dino appears and the battle begins |
| `pet.new_egg` (`confirm`) | gives up on it and starts the next generation: a fresh roll, any species. On the gadget: open STATS, hold X, then enter to confirm |
| `pet.time_scale` (`scale`) | 1 = real time, up to 200, for watching it grow; not saved |
| `pet.debug_stage` (`stage`) | testing: egg, baby, rookie, champion, ultimate or mega |
| `pet.pet` | a stroke, as a tap |

Over the USB console (`>` lines): `pet.stage=champion`, `pet.species=3`,
`pet.ui=stats|train|bubble|battle` for testing, and `chat=...` to type to Muse.

## Code

- `esp32/components/muse/pet.c` the life: genome, needs, sleep, stages,
  evolution, sickness, POWER, persistence (NVS namespace `pet`), chirps in its
  own pitch, reports and Muse's typed replies.
- `esp32/components/muse/avatar/muse_pixel.c` the renderer: the blob, the
  dino body plans, features, face, animations and the egg, from the genome.
  Bonzi's sprite avatar is kept beside it as `muse_pixel.c.bonzi`.
- `esp32/main/gadget_pet.c` the `pet.*` Home Link commands.
- `esp32/components/muse/muse_ui.c` the toy: shell, icons, buttons, bubble,
  stats page, training, themes (`CONFIG_MUSE_PET`).
