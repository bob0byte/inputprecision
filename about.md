# Input Precision
Polls your keyboard/mouse on its own thread (1000 Hz by default) and timestamps every press. Each frame, the physics update is split at the exact moment you clicked, so the input lands on the correct 240 TPS tick even at 60 or 90 FPS. The tick rate itself is unchanged, so gameplay is not made easier.
