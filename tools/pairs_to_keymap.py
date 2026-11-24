"""Name the keys from an any-order pin-pair collection (pairs.csv from kf_collect.ps1) using the
AK820 PRO matrix: 6 row pins x 15 column pins, layout below. Writes keymap.csv (key,pair).

For board SG-8975US-ARGB-1K Ver:02: rows PA10..PA15, columns (left to right)
PA7 PA8 PA9 PB9 PB8 PB17 PB16 PB15 PB14 PB13 PB12 PB7 PB6 PB5 PB4. The script checks that every
collected pair fits this grid and that the occupied grid cells match the layout exactly.
"""
import csv, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', 'pairs.csv')
dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', 'keymap.csv')

ROWS = ['a10', 'a11', 'a12', 'a13', 'a14', 'a15']
COLS = ['a7', 'a8', 'a9', 'b9', 'b8', 'b17', 'b16', 'b15', 'b14', 'b13', 'b12', 'b7', 'b6', 'b5', 'b4']
_ = None
LAYOUT = [  # [row][col]; None = no key at that matrix position
    ['Esc', 'F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'F10', 'F11', 'F12', 'Delete', 'KnobPress'],
    ['Grave', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 'Minus', 'Equal', 'Backspace', 'Home'],
    ['Tab', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 'LBracket', 'RBracket', 'Backslash', 'PgUp'],
    ['CapsLock', 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 'Semicolon', 'Quote', _, 'Enter', 'PgDn'],
    ['LShift', _, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', 'Comma', 'Period', 'Slash', 'RShift', 'Up', _],
    ['LCtrl', 'Win', 'LAlt', _, _, _, 'Space', _, _, 'RAlt', 'Fn', 'RCtrl', 'Left', 'Down', 'Right'],
]

pairs = [r['pair'] for r in csv.DictReader(open(src, newline=''))]
cells = {}
for p in pairs:
    a, b = p.split('-')
    if a in ROWS and b in COLS:
        r, c = ROWS.index(a), COLS.index(b)
    elif b in ROWS and a in COLS:
        r, c = ROWS.index(b), COLS.index(a)
    else:
        sys.exit(f'pair {p} does not fit the row/column grid')
    cells[(r, c)] = p

expected = {(r, c) for r in range(6) for c in range(15) if LAYOUT[r][c]}
extra, missing = set(cells) - expected, expected - set(cells)
for r, c in sorted(extra):
    print(f'unexpected key at row {r} col {c}: {cells[(r, c)]}')
for r, c in sorted(missing):
    print(f'not collected yet: {LAYOUT[r][c]} (row {r}, col {c} = {ROWS[r]}-{COLS[c]})')
if extra:
    sys.exit('layout mismatch - check the layout table')

with open(dst, 'w', newline='') as f:
    w = csv.writer(f)
    w.writerow(['key', 'pair'])
    for (r, c) in sorted(cells):
        w.writerow([LAYOUT[r][c], cells[(r, c)]])
print(f'{len(cells)} keys -> {dst}')
