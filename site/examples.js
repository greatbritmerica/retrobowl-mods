// examples.js - starter rules shown in the builder (plain data; the page and the tests both load it)
(function (root) {
  var EX = [
    { title: 'Tush Push', about: 'On a yard or less to go, a Tush Push button appears. Press it and the ball is snapped: the quarterback keeps it and the line and back drive the pile. Call it three times and they start stacking the middle.',
      rules: [
        { name: 'Offer the Tush Push', ev: 'presnap', conds: [{ v: 'togo', op: '<=', val: 1 }],
          acts: [{ t: 'button', text: 'Tush Push' }] },
        { name: 'They have seen it', ev: 'presnap', conds: [{ v: 'togo', op: '<=', val: 1 }, { v: 'var', name: 'pushes', op: '>=', val: 3 }],
          acts: [{ t: 'tune', key: 'push_hold', op: '+', val: 25, keep: false }, { t: 'msg', text: 'They are stacking the middle' }] },
        { name: 'Tush Push', ev: 'button', arg: 'Tush Push', conds: [],
          acts: [{ t: 'push', val: 100 }, { t: 'speed', group: 'qb', val: 112 }, { t: 'var', name: 'pushes', op: '+', val: 1 }, { t: 'sneak' }] }
      ] },
    { title: 'Victory formation', about: 'Leading late in the fourth quarter, a Kneel button appears. Press it to snap the ball and take a knee.',
      rules: [
        { name: 'Offer the kneel', ev: 'presnap', conds: [{ v: 'quarter', op: '>=', val: 4 }, { v: 'score_diff', op: '>', val: 0 }, { v: 'clock', op: '<=', val: 120 }],
          acts: [{ t: 'button', text: 'Kneel' }] },
        { name: 'Kneel', ev: 'button', arg: 'Kneel', conds: [], acts: [{ t: 'kneel' }] }
      ] },
    { title: 'Hail Mary', about: 'In the last 10 seconds of either half, a Hail Mary button: rushers slow down and the catch becomes a jump ball. Add the Hail Mary script on the Script tab too; it lines your receivers up in the end zone and carries the throw there.',
      rules: [
        { name: 'Offer it before halftime', ev: 'presnap', conds: [{ v: 'quarter', op: '=', val: 2 }, { v: 'clock', op: '<=', val: 10 }], acts: [{ t: 'button', text: 'Hail Mary' }] },
        { name: 'Offer it at the end', ev: 'presnap', conds: [{ v: 'quarter', op: '>=', val: 4 }, { v: 'clock', op: '<=', val: 10 }], acts: [{ t: 'button', text: 'Hail Mary' }] },
        { name: 'Hail Mary', ev: 'button', arg: 'Hail Mary', conds: [],
          acts: [{ t: 'speed', group: 'rushers', val: 80 }, { t: 'tune', key: 'pick_base', op: '+', val: 8, keep: false },
                 { t: 'tune', key: 'whiff_tight', op: '+', val: 10, keep: false }, { t: 'tune', key: 'contest_reach', op: '=', val: 40, keep: false },
                 { t: 'msg', text: 'Hail Mary: aim at the end zone and let it fly' }] }
      ] },
    { title: 'Flags', about: 'Three fouls, each a small random chance: holding on your offense, offside on the defense, and pass interference on a deep incompletion.',
      rules: [
        { name: 'Offensive holding', ev: 'play_end', conds: [{ v: 'chance', op: '<', val: 4 }],
          acts: [{ t: 'penalty', val: -10, replay: true }, { t: 'msg', text: 'FLAG: holding, offense. 10 yards, replay the down' }] },
        { name: 'Defensive offside', ev: 'play_end', conds: [{ v: 'chance', op: '<', val: 3 }],
          acts: [{ t: 'penalty', val: 5, replay: true }, { t: 'msg', text: 'FLAG: offside, defense. 5 yards, replay the down' }] },
        { name: 'Pass interference', ev: 'incomplete', conds: [{ v: 'depth', op: '>=', val: 10 }, { v: 'chance', op: '<', val: 8 }],
          acts: [{ t: 'penalty', val: 15, replay: true }, { t: 'msg', text: 'FLAG: pass interference, defense. 15 yards' }] }
      ] },
    { title: 'Fresh legs', about: 'When the back is gassed, a button brings in a fresh one for three plays. He has full legs but runs a little slower.',
      rules: [
        { name: 'Offer a fresh back', ev: 'presnap', conds: [{ v: 'legs', op: '<', val: 40 }, { v: 'var', name: 'sub', op: '<=', val: 0 }], acts: [{ t: 'button', text: 'Fresh back' }] },
        { name: 'Fresh back comes in', ev: 'button', arg: 'Fresh back', conds: [],
          acts: [{ t: 'legs', op: '=', val: 100 }, { t: 'var', name: 'sub', op: '=', val: 3 }, { t: 'msg', text: 'Fresh back in for three plays' }] },
        { name: 'The backup is a step slower', ev: 'snap', conds: [{ v: 'var', name: 'sub', op: '>=', val: 1 }], acts: [{ t: 'speed', group: 'back', val: 92 }] },
        { name: 'Count his plays down', ev: 'play_end', conds: [{ v: 'var', name: 'sub', op: '>=', val: 1 }], acts: [{ t: 'var', name: 'sub', op: '+', val: -1 }] }
      ] },
    { title: 'Workhorse tax', about: 'Counts carries in a row. From the third straight carry, each one costs the back half again as much.',
      rules: [
        { name: 'Count a carry', ev: 'play_end', conds: [{ v: 'is_run', op: '=', val: 1 }], acts: [{ t: 'var', name: 'carries', op: '+', val: 1 }] },
        { name: 'A pass resets the count', ev: 'play_end', conds: [{ v: 'is_run', op: '=', val: 0 }], acts: [{ t: 'var', name: 'carries', op: '=', val: 0 }] },
        { name: 'Third straight carry costs more', ev: 'presnap', conds: [{ v: 'var', name: 'carries', op: '>=', val: 2 }],
          acts: [{ t: 'tune', key: 'legs_carry', op: '*', val: 1.5, keep: false }, { t: 'msg', text: 'Back has carried twice in a row' }] }
      ] },
    { title: 'They sit on the sticks', about: 'On third or fourth and long, the defense expects a pass: picks are more likely and the defender whiffs less.',
      rules: [
        { name: 'Third and long', ev: 'presnap', conds: [{ v: 'down', op: '>=', val: 3 }, { v: 'togo', op: '>=', val: 8 }],
          acts: [{ t: 'tune', key: 'pick_base', op: '+', val: 6, keep: false }, { t: 'tune', key: 'whiff_tight', op: '+', val: -8, keep: false },
                 { t: 'msg', text: 'Long yardage: they are sitting on the pass' }] }
      ] },
    { title: 'Hurry-up button', about: 'A Hurry up button every play. Pressed, a quick release pays more, but the back gets no rest that play.',
      rules: [
        { name: 'Offer Hurry up', ev: 'presnap', conds: [], acts: [{ t: 'button', text: 'Hurry up' }] },
        { name: 'Hurry up is on', ev: 'button', arg: 'Hurry up', conds: [],
          acts: [{ t: 'tune', key: 'quick_whiff', op: '+', val: 10, keep: false }, { t: 'tune', key: 'legs_rest', op: '=', val: 0, keep: false },
                 { t: 'msg', text: 'Hurry up: quick throws pay more' }] }
      ] }
  ];
  if (typeof module !== 'undefined' && module.exports) module.exports = EX; else root.WS_EXAMPLES = EX;
})(typeof window !== 'undefined' ? window : this);
