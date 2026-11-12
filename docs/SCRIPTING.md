# Play scripts (`rbmod_script.txt`)

A play script is a small program the mod runs during your plays. It can line players up, steer them,
carry a throw, add pre-snap buttons and read or change the game's own values. It needs mod build v89
or later.

- **Where it goes:** `rbmod_script.txt` in the RetroBowl folder in Files (the game's Documents folder).
- **When it's read:** at the start of every play, if the file changed. Nothing to reinstall.
- **Writing one:** the Workshop's Script tab has an editor that checks the script as you type,
  ready-made scripts, this reference and a searchable list of the game's value and routine names.
- **Log:** `rbmod.log` gets `script: self-test passed`, then `script: rbmod_script.txt loaded - N on blocks`
  or `script: rbmod_script.txt NOT loaded - line N: …`.

Example: `examples/rbmod_script.txt` (the Hail Mary), with its button rules in `examples/hail_mary_rules.txt`.
`examples/step1_script.txt` adds a super bullet throw, a screen pass, a stop-and-go button and light injuries; it needs no rules file and can be pasted below the Hail Mary.
From mod build v90, while the script's "Super throw" button is on, a second-finger tap on a receiver as you aim
fires a bullet at him (`src/offtools.c` super throw, flight math in `src/bullet.c`); its tuning is on the Tune tab.

## Shape of a script

Everything lives inside `on <event> … end` blocks. One statement per line; `#` starts a comment.
Indentation is free. You can have several blocks for the same event; they run in file order.

```
on snap
  if down() == 4 and togo() <= 1
    msg("Going for it")
  end
end
```

## Events

| Event | When it runs |
|---|---|
| `on presnap` | Before the snap, once a play. |
| `on button "Label"` | When that pre-snap button is pressed. |
| `on snap` | The moment the play starts. |
| `on release` | The instant the ball leaves your hand (carry() works here). |
| `on frame` | Every frame of your play, from the snap until it ends (60 a second). |
| `on throw` | Just after a pass is out and its landing spot is known. |
| `on catch` | When it happens. |
| `on incomplete` | When it happens. |
| `on intercepted` | When it happens. |
| `on sack` | When it happens. |
| `on touchdown` | When it happens. |
| `on play_end` | After every run or pass play. |

`on catch`, `on incomplete`, `on intercepted`, `on touchdown`, `on play_end` and `landing_x()`/`landing_y()`
come from the Extreme defense code, so they only happen on Extreme difficulty. `on play_end` doesn't
run after a sack. At most 3 pre-snap buttons show, and rules add theirs first.

`on frame` gets a lower step budget than the others (see Limits). Rules (`rbmod_rules.txt`) for the
same event run first, then the script.

## Statements

| Statement | What it does |
|---|---|
| `name = expr` | Set a variable. Numbers and text keep their value from play to play; players and lists are dropped after each block runs. |
| `p.name = expr` | Change a player's game value (a number). `p.x` / `p.y` move him. |
| `if expr` … `elif expr` … `else` … `end` | Run the first part whose condition is true. `elif` and `else` are optional. |
| `for p in list` … `end` | Run the body once for each player in the list. |
| `repeat n` … `end` | Run the body n times. |
| `stop` | End this block now. |
| `func(…)` | Call a function for what it does (a line that doesn't call or set anything is an error). |

## Values and expressions

- **Kinds:** numbers, text in `"double quotes"`, players, lists of players, and `nil` (nothing).
  `true` and `false` are 1 and 0.
- **Operators, loosest to tightest:** `or`, `and`, `not`, comparisons `== != < <= > >=`, `+ -`, `* / %`,
  unary `-`, then `.name` and calls. Parentheses group.
- `+` with text on either side joins them: `msg("Down " + str(down()))`.
- `%` is a true modulo (never negative for a positive divisor). Dividing by 0 is an error.
- **True/false:** 0, `nil`, empty text and an empty list are false; anything else is true.
- `==` compares numbers, text, or whether two players are the same player. Different kinds are never equal.
- `p.name` reads any of a player's game values (`p.x`, `p.y`, or a name from the Workshop's list).
- A variable that was never set is `nil`. Using `nil` in a sum or comparison is an error.

## Functions

Arguments are counted when the script is loaded; a wrong count is a load error.
Counts like 1–7 mean "at least 1, up to 7".

| Function | Values | What it does |
|---|---|---|
| `receivers` | 0 | Your route runners this play (a list). |
| `defenders` | 0 | Every defender (a list). |
| `offense` | 0 | Every player on your offense (a list). |
| `defense` | 0 | Same as defenders(). |
| `deepest` | 2 | deepest(list, n): the n players in the list furthest downfield. |
| `nearest` | 4 | nearest(list, x, y, n): the n players in the list closest to a spot. |
| `sort_y` | 1 | sort_y(list): the list ordered from the top sideline to the bottom. |
| `count` | 1 | count(list): how many are in it. |
| `item` | 2 | item(list, i): one player from a list, counting from 0. |
| `qb` | 0 | Your quarterback. |
| `back` | 0 | Your running back. |
| `ball` | 0 | The ball. |
| `down` | 0 | The down, 1 to 4. |
| `togo` | 0 | Yards to go. |
| `quarter` | 0 | The quarter. |
| `clock` | 0 | Seconds left in the quarter. |
| `lead` | 0 | Your score minus theirs. |
| `goal` | 0 | Yards from the line of scrimmage to the goal line. |
| `los_x` | 0 | The line of scrimmage (x on the field). |
| `fwd` | 0 | Which way you are going: 1 or -1. Multiply downfield distances by it. |
| `field_top` | 0 | The top sideline (y). |
| `field_bottom` | 0 | The bottom sideline (y). |
| `legs` | 0 | The back's stamina, 0 to 100. |
| `landing_x` | 0 | Where your pass in the air comes down (x). |
| `landing_y` | 0 | Where your pass in the air comes down (y). |
| `state` | 0 | The ball's state number (3 = in the air, 5 = someone has it). |
| `since_snap` | 0 | Seconds since the snap. |
| `thrown` | 0 | True while your pass is in the air. |
| `yard` | 1 | yard(n): n yards in field units (1 yard is about 20.5). |
| `endzone_x` | 1 | endzone_x(n): x of a spot n yards into the end zone you are attacking. |
| `random` | 2 | random(a, b): a random number between a and b. |
| `chance` | 1 | chance(p): true p percent of the time. |
| `abs` | 1 | Math. |
| `min` | 2 | Math. |
| `max` | 2 | Math. |
| `floor` | 1 | Math. |
| `round` | 1 | Math. |
| `sqrt` | 1 | Math. |
| `dist` | 4 | dist(x1, y1, x2, y2): the distance between two spots. |
| `str` | 1 | str(v): a number as text, to join with + into a message. |
| `game` | 1 | game("name"): one of the game's own match values (down, scrimmage, minutes...). |
| `set_game` | 2 | set_game("name", n): change one. |
| `global` | 1 | global("name"): one of the game's global values. |
| `set_global` | 2 | set_global("name", n): change one. |
| `call` | 1–7 | call("routine", ...): run one of the game's own routines, with up to 6 numbers or texts. |
| `call_on` | 2–8 | call_on(player, "routine", ...): the same, run as that player. |
| `get` | 2 | get(player, "name"): any of a player's game values (same as player.name). |
| `set` | 3 | set(player, "name", n): change one (same as player.name = n). |
| `place` | 3 | place(player, x, y): put a player on a spot. |
| `steer` | 3–4 | steer(player, x, y [, strength 0-1]): make him run toward a spot this frame. Use it in on frame. |
| `speed` | 2 | speed(player, percent): change how fast he accelerates for this play. |
| `route` | 2 | route(player, "route_hitch"): give a route. Names: route_hitch, route_slant, route_dig2, route_curl, route_post, route_corner, route_streak, route_out2, route_flat. |
| `carry` | 1–2 | carry(x [, arc]): in on release, the throw carries along your aim until it reaches x. arc is how fast it rises (normal deep ball 5). |
| `icon` | 1 | icon(player): a marker at the screen edge pointing to him while he is off screen. Call it in on frame. |
| `icons` | 1 | icons(list): the same for a whole list. |
| `spread` | 4 | spread(list, x, y_from, y_to): line them up evenly across the field at x. |
| `pressed` | 1 | pressed("Label"): true when that pre-snap button is on this play. |
| `button` | 1 | button("Label"): add a pre-snap button (in on presnap). |
| `msg` | 1 | msg("text"): show a message before the snap. |
| `tune` | 2 | tune("setting", n): change a Tune setting for this play (fractions: 0.2 is 20%). |
| `tune_add` | 2 | tune_add("setting", n): add to one for this play. |
| `log` | 1–6 | log(...): write to rbmod.log, to see what your script is doing. |

Coordinates are field units: x runs along the field (multiply downfield distances by `fwd()`), y from
the top sideline (`field_top()`) to the bottom one (`field_bottom()`). `yard(1)` is about 20.5 units.

## Limits and errors

- A block may take 20,000 steps per run (4,000 for `on frame`); past that it stops with
  "the script ran too long (is a loop running away?)".
- Up to 48 `on` blocks, 64 variables and 32 players in a list.
- **Load errors** (bad syntax, unknown function, wrong number of values) keep the whole file from loading;
  the log names the line, e.g. `use == to compare (a single = sets a variable)`.
- **Run errors** (a missing player, `nil` in a sum, an unknown game value) switch off only that block,
  until the script file changes. The log says `error on line N: … - the "on X" block (line M) is off until the script file changes`.
- `game()`, `global()`, `call()` and `.name` reach the game's own values and routines directly. Setting the
  wrong one can break a game in progress; `log()` a value before changing it.
