# Snekagon: the 7 turn game that could not be solved

 · 

## How to read this draft

First full draft of the narration, about 26 minutes at a calm 3Blue1Brown pace (roughly 3,400 spoken words); we will probably want to trim it. Every number in it comes from the engine logs; the fact sheet at the end lists them.

Plain paragraphs are the voice-over, written to be read aloud.

**Visual:** lines describe what is on screen while that paragraph plays.

**[ADAM: …]** marks facts only you know (years, names, quotes). Nothing there is invented; fill or cut.

Each section has a rough running time, so we can see where the video drags.

## 1. Cold open (0:00–0:50)

**Visual:** An empty hexagonal board, 91 cells, drawn one ring at a time from the centre outwards. A single red hex drops into the middle cell.

Here is a game you can explain in under a minute. Two players, one board, a handful of small pieces each. You take turns placing them. Each turn, either start a new snake that doesn't touch any other pieces, or grow one of your snakes. Whoever ends up with the longest snake wins.

**Visual:** A full game plays itself out at a calm pace, about one move per second; the first moves carry small tags, “start a new snake: touches no other pieces” and “grow one of your snakes”. Snakes grow, curl, get blocked. Final position freezes. Blue wins, 22 to 18.

Each player gets just seven real turns. The whole game fits on a coffee table and takes about 5 minutes.

**Visual:** Zoom out. The board shrinks into the corner while a tree of possible moves explodes out of it and fills the screen. Below it, a row of 76 small squares: the moves in the order the engine ranks them, from its favourite (1st) on the left; the 76th lights up in gold.

So here is the question that has been bugging me for years. If both players play perfectly, who wins? It feels like a question a computer should answer in an evening. This video is about why it did not, what we found when we tried anyway, and a move the engine had ranked only 76th. One move that seemed weak at first, but changed the whole picture.

**Visual:** Title card: Snekagon: the 7 turn game that could not be solved.

## 2. Who I am (0:50–1:30)

**Visual:** Adam on camera, or a slow pan over the boxes of Project L and Drillers on a shelf.

Hi, I am Adam, a Czech game designer. You may know me for my games Project L or Drillers. But today, I would like to tell you about one of my earlier games, much less known, but no less interesting: Snekagon.

**Visual:** Photos: the published game from Maty Moves (wooden pieces, a leather board, a cloth bag), then the prototype it started as, a paper board with printed pieces.

This is a story about a game that didn't sell well, a story about a question I could not answer, a story about player psychology, and a story about the deep beauty and mystery of board games.

## 3. How it started (1:30–2:40)

**Visual:** Hand-drawn hex grid on paper, coloured pencils filling cells. Beside it, a close-up of the prototype board; then the pencil grid grows into the real board.

The idea came out of thin air. I was riding home from a long, demanding game night back in 2019, and I wanted something completely elementary, as natural and simple as a game can get. Somewhere on that tram ride I caught it, and I did not let go.

The first games surprised me. The rules are tiny, but the board fills up fast, and every piece you place is a threat and a wall at the same time.

**Visual:** A page from my diary, February 2020, written the evening after a playtest at CGE. The lines appear as I read them.

After a playtest at CGE in February 2020, where it was played a few times, I wrote this in my diary: “This game keeps fascinating me. An area-control abstract that lasts eight moves? That can’t work. But it does. You can never calculate more than seven moves ahead, simply because there will never be that many moves left. It is about planning and intuition, and every game is different.”

**Visual:** The board with a few pieces on it. A counter in the corner ticks: 1 hex, then 7 turns for each player. Then a row of games that have been solved, each with its year and result: tic-tac-toe (draw), Connect Four (1988, first player wins), checkers (2007, draw), Othello (2023, draw). Snekagon at the end of the row, with a question mark.

And the natural question followed: who wins? This game is small. Ninety-one cells, seven turns each. It has no dice, no cards, no hidden information. So somewhere out there is a definite answer: either the first player wins, or the second player wins, or perfect play ends in a tie. It really feels like a game that should be easy to solve.

## 4. Out into the world (2:40–3:50)

**Visual:** A short timeline: Gigamic declines in December 2019, Maty Moves publishes the game in 2020, with a photo of the published game.

I pitched the game around. In December 2019, Gigamic, the publisher of Quarto and Squadro, turned it down. The strategy part seemed a bit too light for them. I tried to ask what they meant by that, or which winning strategies they found, but never heard back. In the end I published it with a small Czech publisher Maty Moves in 2020.

**Visual:** The rejection email from December 2019, names and addresses blurred, “a little bit too light” highlighted. Underneath, Adam’s reply types itself out.

**Visual:** Snekagon’s BoardGameGeek entry: an average of 4.8 from 13 ratings, the ratings breakdown growing bar by bar, and beside it the list of ratings as it looks on the site, usernames and avatars blurred.

The reception was lukewarm, to put it kindly. And one comment kept coming back, in different words: the first player has an advantage you cannot beat.

**Visual:** The two comments appear one after the other as they look on BoardGameGeek; the bold phrases light up as they are read. No usernames.

[QUOTE]

[QUOTE]

To be clear, this video is not here to prove anyone wrong. The game did not land well with players, and that's OK. I have no right to tell players how they are supposed to have fun playing board games.

## 5. The rules (3:50–6:20)

**Visual:** The board again, 91 hexagonal cells in a big hexagon with six cells on each side.

The board is a hexagon of 91 cells. Each player has a set of pieces made of hexes: a domino, two shapes of three, and four shapes of four.

**Visual:** The pieces fan out: 2, 3I, 3V, 4I, 4A, 4B, 4C. Each one spins and flips over to show that every rotation and mirror image is allowed. A small 1-hex sits apart in red.

The first player, red, also gets this: a single hex. Red places it anywhere to start the game. It is such a tiny move that it does not really count as a turn, which is why we say each player then gets seven turns.

**Visual:** Two tallies appear under the board: red 1 + 2 + 3 + 3 + 4 + 4 + 4 + 4 = 25 cells, blue 24 cells.

So red owns 25 cells of material and blue owns 24. Remember that difference; the players certainly did.

On your turn you place one of your pieces, and there are exactly two ways to do it.

**Visual:** A blue piece drops onto an empty area, far from everything. Its outline pulses green.

One: start a new snake. Then the piece must not touch any other piece at all, of either colour.

**Visual:** A red piece slides in and attaches to the end of a red snake. Then another red piece bridges two red snakes into one.

Two: grow. The piece touches at least one of your own pieces. It is allowed to touch the opponent as well, and it may join two of your snakes together.

**Visual:** Three red cells form a tight triangle and flash red with a cross. Then a red Y-shaped branch flashes red with a cross.

Whatever you do, your cells must stay snake-shaped: no cell may have more than two of your own neighbours, and three of your cells may never all touch each other. No branches, no blobs, no sharp turns. Every group of your cells is a path, or a closed ring.

**Visual:** A player with no legal placement: their remaining pieces grey out and the word “pass” appears.

If you cannot place anything, you pass. When neither player can move, the game ends. You measure your longest snake in cells, and the longer snake wins. Equal lengths are a tie.

That's the whole rulebook.

## 6. Where the strategy hides (6:20–9:30)

The rules are short, but they hide a surprising amount of play. Three ideas carry most of it, and all three show up in the game we just watched.

**Visual:** Each idea gets its own short board animation with one highlighted move and a small caption in the corner: “1. Blocking”, “2. Joining snakes”, “3. Flowing around”.

### Blocking

**Visual:** The game from the rules, after three moves: red’s 1-hex at the top and a second red snake at the bottom. Blue’s U-shaped piece lands right against one end of that second snake; the cell in front of that end is crossed out: dead end.

The most direct weapon is a wall. Red has just started a second snake. Blue answers with a U-shaped piece right against one of its ends, and that end is dead: there is nowhere left for it to grow.

### Joining snakes

**Visual:** Red’s two snakes, the 1-hex grown into five cells at the top and the second snake of seven at the bottom, each outlined. A single red two-piece drops in between and they become one snake: the counter jumps from 7 to 14.

Starting a new snake looks like a wasted move, because it adds nothing to your longest snake. But it can be an investment. Red’s second snake started far away from its first, and on move nine a single two-piece joins the two: seven cells become fourteen in one move.

### Flowing around

**Visual:** Right after red’s joining two-piece, every empty cell next to the middle of red’s snake lights up in blue: red can never use these cells, blue can. A small inset jumps ahead to the end of the game, where blue’s very last piece lands on two of them, up in the corner (move 14). Then the game goes on, and every empty cell one player can never use again is tinted in the colour of the player who still can, until the free board is split into pockets.

Walls also work the other way round. The cells right next to the middle of a snake are lost for its owner, since a piece there would give a body cell a third neighbour, but for the opponent they are perfectly good: reserved, to be used whenever needed. In this game, two cells beside red’s snake, up in the corner, belong to blue from now on, and blue’s very last piece goes straight into them. As the board fills, these dead cells spread and split the free space into pockets. Late in the game the question is not how many pieces you have, but how many cells you can still reach.

**Visual:** The final position of that game. Red still holds its straight four-piece; it tries all five places where it would fit, and each time the cells that break the rules flash up.

## 7. The question that would not go away (9:30–11:00)

**Visual:** Adam on camera (the Lost Ruins of Arnak and SETI boxes can be in the shot).

Throughout the years, I focused on other projects at CGE: joining the Arnak team for all the expansions, working on SETI and its expansion, and more. But this game stayed in the back of my mind all those years.

**Visual:** A small board that looks easy; a minimax search tree that soon grows far too wide and gets crossed out; two strong players with question marks.

Snekagon is just seven turns on a fairly small board. It looks like a game that can be solved easily. I gave it a few shots, and always failed. I tried some basic minimax search algorithms that I had a general idea about, but that was just not enough to tackle this, and long discussions with strong players of abstract games did not crack it either. At that point, I knew this would be quite a problem to solve.

**Visual:** A timeline from Snekagon’s release in 2020 through ChatGPT, Claude, Gemini and the later Claude models, each attempt crossed out, until Opus 5.5.

Meanwhile, the advent of large language models was unfolding. Of course I asked them to solve the game, and none of them could. Then Opus 5.5 came out, and I decided to try one more time. Not by asking for the answer, but by asking it to build the tools to find one.

## 8. Teaching a machine to play (11:00–13:30)

**Visual:** A terminal window, then the browser UI: the board, a win-probability bar for each candidate move.

The first step was not solving the game. It was getting a program that plays it well. In the first session we had a fast rules engine, a web board to play on, and a first opponent.

**Visual:** Monte Carlo playouts: from the current position, hundreds of random games fan out as faint lines; each ends in a tiny red or blue dot.

That first opponent used Monte Carlo tree search. It plays thousands of random games from each position and prefers moves that tend to win. It sounds clever, but I beat it comfortably as blue. Random games simply do not understand walls: out of two thousand legal moves, often only a handful really matter, usually walls, and random games cannot tell them apart from the rest. It let itself get boxed in again and again. It turned out that the tree is simply too wide for Monte Carlo to be effective.

**Visual:** A search tree a few levels deep. Most branches fade out as alpha-beta cuts them; a heat map over the board shows “who gets where first”.

The second engine looked ahead properly, with alpha-beta search, but judged positions with a hand-made rule, and it did even worse: it lost to the random games, 2 to 10.

**Visual:** A scatter of solved positions, each a red or blue dot; a logistic curve fitted through them, labelled “P(win)”.

So instead of guessing what matters, we let the game tell us. The exact solver, which we will meet in a moment, can settle positions late in the game. We solved tens of thousands of such positions and fitted a simple model, a logistic regression over 13 features of the position, to predict who wins. The features are simple things: the length of each player’s longest snake, how many snake ends can still grow,  who reaches which empty cells first, or how many separate snakes each player has. With that learned judgement the engine beat the random-playout engine 16 games to 0, and it beats me comfortably every time.

## 9. Trying to solve it (13:30–16:30)

Playing well is one thing. Solving means something much stronger: proving the result of perfect play, with no guessing anywhere.

**Visual:** One position, rotated and mirrored into 12 copies that collapse back into one: 91 first moves become 12, blue’s 2,132 replies to the centre become 172. Then the game tree grows from the root anyway: about 1,600 replies, then 930, 680, 360, until it fills the screen.

How big is the tree? First, the cheap trick. The board looks the same after six rotations and a mirror, so up to twelve positions are really one. That leaves red just 12 truly different cells for the 1-hex, and after the centre opening, blue has 172 truly different replies instead of 2,132. But symmetry only helps at the very start. A few moves in, positions are almost never symmetric, and the numbers keep coming: around 1,600 replies for red, around 900 for blue after that. They shrink as the board fills, but they multiply. Even counting rotated and mirrored games as one, sampling random games suggests something like 10^29 different ways a game can unfold. You have no chance to look at them all.

So the solver has to be clever about which parts it never looks at. Four tricks do almost all of the work.

**Visual:** The hex board next to its 11 × 11 bitmap, and below it the two 128-bit numbers (red and blue), each as a strip of bits and in hex. A few more positions flash by, the bitmaps and numbers changing with them. Then the neighbours of red’s snake light up on the board, in the bitmap and in the bits.

One, speed. The whole board fits into a single 128-bit number, one bit per cell. Every position the solver meets becomes a pair of such numbers, one for red and one for blue. Finding all the neighbours of a snake is a handful of bit shifts, and the legality of a piece in every possible spot is checked at once.

**Visual:** Two different move orders arrive at the same position; the second one hits a stored card that says “already solved: blue wins”.

Two, memory. Different move orders often reach the same position, so every result is stored and reused.

**Visual:** Red’s snake with its current length 9 and, in a faint halo, every cell it could still reach: at most 14. Blue’s sure length: 15. The position is stamped “decided”. (Illustrative numbers.)

Three, bounds. This one is specific to Snekagon. A snake never gets shorter, so its current length is a sure lower bound. And we can compute a sure upper bound: the cells a player can still reach, filled with the pieces they still have. If blue is already guaranteed more than red could ever get, the game is over, even with pieces left to play.

**Visual:** Candidate moves lined up by the learned evaluation; the first one is tried, and the rest of the line greys out.

Four, order. The solver only needs one winning move to prove a win, so it tries the most promising moves first, ranked by the same learned judgement that powers the engine.

**Visual:** A counter racing up to 11 billion, then stopping. Stamp: “red to move loses, all 956 moves”.

With all of that, the first real test: a position after four moves. The solver checked every one of red’s 956 distinct moves, 11 billion positions in total, and proved that red loses after each of them. That took about ten hours of single-core computing. For one position, four moves into a fifteen-move game.

## 10. One opening, 1,589 replies (16:30–19:00)

**Visual:** The 12 truly different cells for red’s 1-hex light up; the centre glows brightest. Blue’s best reply, the 4B piece at the top, appears with the engine’s estimate: 65 % for blue, and below it what players say, “the first player wins”, with a question mark.

Solving the whole game at once was out of reach, so we picked one line and tried to prove it. Red opens in the centre. The engine’s favourite answer for blue is this four-piece at the top, which it rates at about 65 % for blue; the next best reply sits around 50 %. This was our first glimpse of a real result, and it goes straight against what players think: they say the first player wins, but it may very well be the other way around. It seemed that blue had the better chances, so we tried to actually prove that blue always wins with perfect play.

**Visual:** A fan of 1,589 red replies below the position, each a small card. One card flips over: under it a single blue answer, and under that a full fan of red follow-ups, all marked with a blue tick.

What does that take? Red has 1,589 genuinely different replies. For every single one of them we need one blue answer, and then a proof that every red follow-up loses. Each of those follow-ups is a position after five moves, a job one processor core can finish in a few minutes, or in a few hours.

**Visual:** My home laptop, working day and night: job cards stream into it while a sun and a moon take turns above it.

So I put my home laptop on it, working day and night.

**Visual:** The fan of red replies; cards turn blue one by one. A counter: 16 proven.

And it worked. Red reply after red reply fell, almost always to the very first blue answer the engine suggested. Sixteen replies proven in a week, each costing between 13 and 241 hours of computing, about 60 on average. That sounds like progress, until you do the division: sixteen out of 1,589 is one percent. At that pace, this one opening line would keep my laptop busy for a year or two.

**Visual:** The hardest of the sixteen proofs, 241 hours (red’s 3V:29,40,50, blue’s winning answer 4A:6,7,8,19). Red’s move 5 grows its snake (the closest of its growing moves); after that both sides play perfectly for the score. Both second snakes marked, then blue’s straight four down the middle outlined in gold as it joins blue’s two snakes. Score: 21 : 15.

The toughest of the sixteen took 241 hours on its own. Red answers with a small bent three-piece that runs from the centre straight at blue’s snake, and blue answers in the top corner. Red grows its snake, and from there both sides play for every cell. Each of them builds a second snake. Then blue plays a straight four right down the middle, between red’s two snakes, and it joins blue’s own two snakes into one. Blue wins, 21 to 15.

**Visual:** One card refuses to flip. It is red reply 3V. Forty blue answers are tried under it, one after another, and each is crossed out.

It started to look like blue actually wins. But then we hit this red reply. Red plays a small bent three-piece that runs its snake from the centre straight towards blue’s snake. The solver tried blue’s 40 most promising answers. All 40 failed. Not by guesswork: for each one, the solver found a red reply and proved that blue cannot win after it. In 31 of the 40 cases, blue actually loses. In the other 9, red’s reply holds blue to a draw. That one red reply alone ate 282 hours of computing.

## 11. The game hidden behind move 76 (19:00–21:00)

Reference for the animation: the step-through viewer of this game, with every move and both snake lengths.

**Visual:** The position after red’s bent three-piece. Blue’s U-shaped piece lowers onto the board and wraps around red’s 1-hex.

Let’s look at the very first answer blue tried, because it is the most natural one. Blue wraps a U around red’s starting hex. The centre end of red’s snake is dead.

**Visual:** Red’s straight four-piece extends the other end, curling around. The board dims and a stamp appears: “exact value: draw”.

Red answers by growing from the other end, and the solver proves that this position is exactly a draw. With perfect play from here, neither side can force a win.

**Visual:** The draw position. Red’s straight four is outlined and the two cells beside blue’s U glow red: reserved for red. A blue wall appears as a ghost right under the two cells (the engine’s 8th choice, 4A:89,90,91,102); red’s answer flows into the two cells and turns left around it (4A:58,69,80,81). Then the bare position with a pause sign, a big 3, 2, 1, and the 76th move drops in, in gold. The queue of blue’s moves in engine order (75 red, the 76th gold), then moves 7 to 10: red flows into its cells and cuts off one end of the new snake, blue’s three-piece joins the two snakes and the counter jumps from 8 to 15.

Red’s answer is quietly brutal. Red now has these spaces reserved. Red has a free road into the bottom of the board. The natural answer is to wall it off, right under red’s two cells, and the engine rates that highly too. But red simply turns the other way and flows around the wall to the left, and blue loses, 15 to 16.

So here is a challenge for you. Pause the video right here, and try to find a move for blue that does not lose, or at least one that feels strong. Ready? Three, two, one.

Here it is. Did you get it? If not, don’t feel bad. The engine, which beats most humans comfortably, ranked this move only 76th, and all 75 moves it liked better lose. And here is why the 76th works. Blue starts its second snake not in the open space at the bottom, but on the left, close enough to the top end of its first snake that a single three-piece can join them. Red, though, still threatens to grow into the bottom of the board. Red does flow into its two cells and cuts one end of the new snake off. Blue’s next piece, an L at the bottom, looks almost random, but it is not. Of blue’s 159 possible moves here, only 21 keep the draw, and every single one of them goes into the bottom of the board: blue has to claim that space before red does. Then blue’s three-piece joins the two snakes: eight cells become fifteen.

**Visual:** The game plays out to the end, quickly and without comment. Final counters: 17 and 17.

From there, the game ends in a draw, seventeen to seventeen.

**Visual:** The camera zooms onto the 76th move, labelled “ranked 76th”.

Think about what that means for a human player. The move that saves the game is one that a strong engine ranks 76th. A person sitting at the table would almost never find it, and would never know it was there.

## 12. Why it is so hard (21:00–23:00)

So, why is it so hard to prove? A proof of a win has a lopsided shape, and that shape is the whole problem.

**Visual:** A tree that alternates between two kinds of levels. On blue’s levels a single branch is highlighted; on red’s levels every branch is highlighted. Labels: “blue needs one move”, “red: all moves must fail”.

To prove that blue wins, blue needs just one good move at each of her turns. But she has to answer every single move red could make. One branch, then all branches, then one, then all. The “one” levels are cheap if you guess right. The “all” levels never are.

[DIAGRAM]

Numbers from our opening line: 1,589 red replies, about 1,000 blue moves under each, and 600 to 1,100 red follow-ups for every blue answer we test.

**Visual:** Under the stubborn red reply, a queue of 1,032 blue moves. Forty are crossed out; 992 are still grey.

And guessing right is exactly what failed us. Under that stubborn red reply, blue has 1,032 different moves. To prove that blue cannot win there, we have to refute every one of them. The 40 most promising are done; 992 are left. Finishing them all would take my laptop anywhere from a few days to a few weeks.

**Visual:** Three bars: “this one red reply: days to weeks”, “this opening line: a year or two, if blue wins”, “the whole game: a decade or more”.

Scale that up. If the centre opening is a blue win, proving it at the pace we measured is a job of a year or two on my laptop. Proving the result of the whole game means doing that for every one of red’s 12 different first moves: a decade or more. And if the true answer is a draw, it is worse still: a draw has to be proven from both sides, so the “all moves” levels appear for both players.

The deepest reason why this game is so hard to solve is the one we saw at move 76. The solver is fast because good guesses let it skip almost everything. In this line, the decisive move was one nobody would guess, and nothing tells us it is the only place where the game hides one. And that is exactly why we need to prove every single one of red’s replies: we never know where a move that is rated weak is actually the strongest.

## 13. Human intuition versus the cold machine (23:00–25:00)

**Visual:** Adam on camera at the table, pointing out red’s extra piece (“the answer is sitting right on the table”); from “If both players simply build”, the peaceful game on the board.

So why did players feel that the first player cannot lose? I think the answer is sitting right on the table. The starting player takes the center, and has one more piece. If both players simply build their own snakes as long as they can, red wins by exactly one cell. That is the game most people play at first, and in that game the feeling is correct. Red also moves first and last, so it always seems to have the last word.

**Visual:** The same position twice, side by side: on the left both players build peacefully and red wins 25 to 24; on the right blue walls off red’s centre and the counters swing.

What the numbers show is a different game, one where blue blocks. In the opening line we studied, blue wins by force against 16 of the 17 red replies we finished, usually with the very first move the engine suggests. Against the 17th it is the other way round: blue is the one clinging on, and in the line we followed only a move ranked 76th saved her.

So were the players wrong? Not really. A game is not its game tree. A game is what people experience at the table, and if a game feels decided, it is decided for them. Theoretical balance means nothing when the balancing moves are invisible. Let me say that again. Theoretical balance means nothing when the balancing moves are invisible. And just because a game is perfectly balanced, that does not guarantee it is fun. What makes a game fun is a whole other big open question, one that game designers have been trying to answer for decades.

**Visual:** A slow zoom onto the 76th move on the board. Then a Go board: a close-up of one corner that slowly zooms out while the moves of AlphaGo vs Lee Sedol, game 4 (2016), are added, ending on Lee Sedol’s move 78 in gold.

And that leaves me with a question I find genuinely interesting. Is a game where every result hangs on a single hidden cell a deep game, or a frustrating one? Perhaps it is both, depending on who is sitting across from you. It almost feels like the game of Go in some ways: you know there is an astronomical number of possible games, and it could hypothetically be calculated. But in the end, you must read your opponent, trust your intuition, and feel the patterns.

## 14. What we know (25:00–26:30)

**Visual:** Three cards, one at a time.

So where does this leave us? Three things I am now sure of.

For human players, the game is extremely tight. Results swing on a single cell, and the saving move can be one that even a strong engine ranks 76th.

An exact proof is extremely expensive. Hundreds of hours of computing went into a single opening line, and that line is still open.

It is not at all clear how the game ends with perfect play. Blue looked like the favourite, until one red reply refused to fall.

**Visual:** The board with the 1-hex in the centre, and the question mark from the solved-games row settling over it.

Seven turns. Ninety-one cells. And still, nobody knows who wins.

**Visual:** A browser window at boltkey.github.io/snekagon-lab: red plays the centre, the engine answers, the cursor browses its analysis (each row previews its move on the board), then a late position is solved exactly. Beside it, a photo of the published game. End card: where to play, where to buy the game, and that the engine and solver were built with Claude Opus 5.5.

If you want to play against the engine yourself, right in your browser, or analyse any position, click the link in the description. To buy the game, visit Maty Moves. And maybe one of you will find the next move 76. Thanks for watching.

## Fact sheet

Every number in the script, where it comes from, as of 8 October 2026. “Thread-hours” means hours of one processor core.

Fact

Value

Source

Board

91 cells, hexagon with side 6

RULES.md

Material

red 8 pieces, 25 cells; blue 7 pieces, 24 cells

RULES.md

Red’s first move

91 cells, 12 different up to symmetry

engine

Blue’s replies to the centre

2,132 legal, 172 different up to symmetry

engine

Legal moves per turn, random games

about 2,100, 1,600, 930, 680, 360 … for turns 2–6

1,500 random games

Size of the game tree

about 10^30 games; about 10^29 counting rotated and mirrored games as one

Knuth estimate from 20,000 random games (10^29.8 and 10^28.7, standard error about a third)

First engine vs hand-made alpha-beta

10 : 2 for the random-playout engine

results/arena_ab_vs_mcts_1s.txt

Learned alpha-beta vs random-playout engine

16 : 0

results/arena_ablearned_vs_mcts_1s.txt

Learned evaluation

logistic regression, 13 features (12 measured on the board, plus whose turn it is) and a constant, tens of thousands of solved positions

tools/fit_eval.py, data/

Engine’s view of blue’s 4B reply

about 65 % for blue; next best about 50 %

engine analysis

First solved position (after 4 moves)

red loses after all 956 moves, 11.0 billion positions, about 10 thread-hours

results/pilot_ply4.out

Red replies after centre + 4B

1,589 different

coordinator

Proven so far

16 red replies, 13 to 241 thread-hours each; 15 of them fell to blue’s first candidate; about 60 on average, so 1 % of all red replies after one week

coordinator table

Compute spent on the line

about 1,300 thread-hours (18,463 jobs, 253 s average)

coordinator status, 7 October

Stubborn reply 3V:40,41,50

40 blue answers refuted (31 losses, 9 draws), 282 thread-hours, 992 of 1,032 blue moves untested

coordinator

U-block line

exact draw, 17 : 17; the saving move is 76th by evaluation, the 75 before it lose

solver run

Sample of 24 untested blue moves

14 refuted, 10 unresolved, none winning

sample run

Time to settle 3V:40,41,50

a few days to a few weeks on my laptop

estimate from the sample

Solved games row

tic-tac-toe draw; Connect Four 1988, first player wins; checkers 2007, draw; Othello 2023, draw

from memory, please verify

Story details in sections 3, 4, 7 and 13 (the origin, the CGE playtest, Maty Moves, the Board Game Arena version, the tester’s comment) come from Adam’s diary entries from January to December 2020 and the Gigamic email exchange from December 2019.

### Open items for Adam

Release year with Maty Moves (2020?)

Gigamic (December 2019): OK to show or quote the email? Did they ever reply?

Board Game Arena version: did it launch?

Typical play time

Diary quotes: check the translations, decide whether to show the pages on screen

Outro, credits, whether the code or viewer goes public