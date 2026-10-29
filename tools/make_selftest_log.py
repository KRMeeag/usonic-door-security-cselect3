#!/usr/bin/env python3
"""
Generate a synthetic capture file that mimics the firmware's serial output.

This exists so tools/analyze.py can be verified without the hardware, the door,
or a test partner — useful when someone picks the analysis up cold:

    python3 tools/make_selftest_log.py
    python3 tools/analyze.py logs/synthetic_selftest.log

The run is built with 10 positive and 8 negative trials, and three of the
positive dips are deliberately too short for the midterm persistence rule, so a
correct analysis reports 7 TP / 3 FN / 0 FP / 8 TN. Any other result means
analyze.py has regressed.

THIS IS NOT REAL DATA. Never put its output in the paper.
"""
import random, os
random.seed(7)
CYC, THR = 300, 30.0
ms = 0
out = []
def t(m):
    s = m / 1000.0
    return f"{int(s//3600):02d}:{int(s//60)%60:02d}:{s%60:06.3f} > "
def line(s):
    out.append(t(ms) + s)

line("#H,V,1,door-intrusion")
line("#H,D,ms,state,armed,alarm,d1,d2,d3,hmask,omask,tmask,thr,streak,risk,snd,sndev,temp,hum,dhtok,rssi,heap")
line("#H,E,ms,type,detail")
line("#E,0,boot,DISARMED")
line("[boot] Door Intrusion Detection")      # human-readable noise, must be ignored

heap, sndev = 232000, 0
state, armed, alarm, streak = "DISARMED", 0, 0, 0

def sample(d, omask, snd=0):
    global heap
    heap -= random.randint(0, 3)
    line(f"#D,{ms},{state},{armed},{alarm},{d[0]:.1f},{d[1]:.1f},{d[2]:.1f},"
         f"7,{omask},0,{THR:.1f},{streak},0,{snd},{sndev},28.4,61.0,1,-57,{heap}")

def idle(n):
    global ms
    for _ in range(n):
        sample([280 + random.gauss(0, 1.6) for _ in range(3)], 0)
        ms += CYC

idle(10)
line(f"#E,{ms},arm,b=280/281/279 thr=30")
state, armed = "ARMED", 1
idle(8)

# --- positive trials: door opens, dip of varying length -------------------
# dips sized in cycles; 3+ sustained cycles is what the midterm rule needs
for dip_cycles, cls in ((4,"held"),(3,"held"),(2,"fast"),(5,"held"),(4,"held"),
                        (1,"fast"),(3,"fast"),(4,"held"),(2,"fast"),(4,"held")):
    line(f"#E,{ms},trial_pos,{cls}")
    ms += CYC * random.randint(2, 4)          # human reaction before opening
    for k in range(dip_cycles):
        sample([18 + random.gauss(0, 2) for _ in range(3)], 7)
        streak = min(streak + 1, 9)
        ms += CYC
        if streak >= 3 and not alarm:
            line(f"#E,{ms},alarm,d=18/19/17 thr=30")
            alarm, state = 1, "ALARM"
    idle(4)
    if alarm:
        line(f"#E,{ms},disarm,app")
        alarm, state, armed, streak = 0, "DISARMED", 0, 0
        idle(3)
        line(f"#E,{ms},arm,b=280/281/279 thr=30")
        state, armed = "ARMED", 1
    streak = 0
    idle(5)

# --- negative trials: single-beam waves, sound-only, partial pushes --------
for kind in range(8):
    line(f"#E,{ms},trial_neg,neg")
    ms += CYC * 2
    if kind % 3 == 0:                          # one beam only
        for _ in range(3):
            sample([19.0, 280.0, 281.0], 1); ms += CYC
    elif kind % 3 == 1:                        # sound only
        sndev += 2
        line(f"#E,{ms},warn,sound-no-door")
        for _ in range(3):
            sample([280 + random.gauss(0, 1.6) for _ in range(3)], 0, 1); ms += CYC
    else:                                      # two beams, briefly
        for _ in range(2):
            sample([20.0, 21.0, 279.0], 3); ms += CYC
    idle(6)

idle(40)
os.makedirs("logs", exist_ok=True)
open("logs/synthetic_selftest.log", "w").write("\n".join(out) + "\n")
print(f"wrote logs/synthetic_selftest.log  ({len(out)} lines, {ms/1000:.0f}s simulated)")
