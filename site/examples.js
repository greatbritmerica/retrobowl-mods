// examples.js - starter rules shown in the builder (plain data; the page and the tests both load it)
(function (root) {
  var EX = [
    { title: 'Short-yardage surge', about: 'On a yard or less to go, a Surge button appears. Press it and your line and quarterback fire off faster at the snap.',
      rules: [
        { name: 'Surge button on short yardage', ev: 'presnap', conds: [{ v: 'togo', op: '<=', val: 1 }],
          acts: [{ t: 'button', text: 'Surge' }, { t: 'msg', text: 'Short yardage: Surge is available' }] },
        { name: 'Surge at the snap', ev: 'snap', conds: [{ v: 'pressed', name: 'Surge', op: '=', val: 1 }],
          acts: [{ t: 'speed', group: 'line', val: 125 }, { t: 'speed', group: 'qb', val: 115 }] }
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
      ] },
    { title: 'Heat check', about: 'After a touchdown pass, your next drive starts with a sharper offense for one play. It catches two times in three.',
      rules: [
        { name: 'Touchdown lights the fuse', ev: 'touchdown', conds: [], acts: [{ t: 'var', name: 'hot', op: '=', val: 1 }] },
        { name: 'Hot: receivers fly', ev: 'snap', conds: [{ v: 'var', name: 'hot', op: '=', val: 1 }, { v: 'chance', op: '>=', val: 33 }],
          acts: [{ t: 'speed', group: 'receivers', val: 112 }] },
        { name: 'Hot: used up', ev: 'play_end', conds: [{ v: 'var', name: 'hot', op: '=', val: 1 }], acts: [{ t: 'var', name: 'hot', op: '=', val: 0 }] }
      ] }
  ];
  if (typeof module !== 'undefined' && module.exports) module.exports = EX; else root.WS_EXAMPLES = EX;
})(typeof window !== 'undefined' ? window : this);
