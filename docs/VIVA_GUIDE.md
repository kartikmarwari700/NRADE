# NRADE: Teacher ko samjhane ki guide

## 1. 30 second mein project

> "Sir, network mein bahut saare nodes (router, host) limited resources ke liye
> compete karte hain: bandwidth, buffer, port, channel. Agar har node kuch
> pakad kar doosre ka wait kare to deadlock ho jata hai. Humne C mein ek
> simulator banaya hai jisme har node ek thread hai. Iske do mode hain:
> **Avoidance** (Banker's Algorithm, deadlock hone hi nahi deta) aur
> **Detection** (request free mein deta hai, ek detector thread deadlock
> pakadta hai aur recovery karta hai). Aakhir mein dono ka comparison milta hai."

## 2. Real life example

Chauraha jam: har gaadi apni jagah pakde hai aur aage wali jagah ka wait kar
rahi hai. Koi aage nahi badh sakta.

* **Avoidance:** traffic police pehle hi sochta hai, "is gaadi ko andar aane
  diya to jam ho sakta hai?" Agar haan, to rok deta hai.
* **Detection + recovery:** sabko aane do. Jam ho gaya to ek gaadi ko peeche
  karwao.

## 3. Basic theory

**Deadlock ki 4 zaroori conditions (Coffman)**, charon saath honi chahiye:

1. **Mutual exclusion**: ek instance ek time pe ek hi node ke paas.
2. **Hold and wait**: kuch pakda hua hai aur aur maang rahe ho.
3. **No pre-emption**: zabardasti cheen nahi sakte.
4. **Circular wait**: N1 → N2 → N3 → N1.

| Tareeka | Matlab | Project mein |
|---|---|---|
| Prevention | 4 mein se ek condition kabhi hone na do | alag mode nahi hai |
| Avoidance | har request se pehle check karo ki state safe rahegi | Banker's Algorithm |
| Detection | hone do, phir pakdo | WFG + DFS, Work/Finish |
| Recovery | deadlock todo | victim ko pre-empt ya terminate |

> Agar teacher pooche "prevention kahan hai?" to seedha bolo: "Alag prevention
> mode implement nahi kiya. Recovery mein pre-emption hai, jo 'no pre-emption'
> condition ko todta hai." (Synopsis ke title mein "prevents" likha hai, isliye
> yeh sawal aa sakta hai.)

## 4. Data structures (M = resource types, N = nodes)

| Naam | Matlab |
|---|---|
| `total[M]` | resource ke kul instances |
| `available[M]` | abhi free |
| `max[N][M]` | node zyada se zyada kitna maangega |
| `alloc[N][M]` | abhi node ke paas |
| `need[N][M]` | `max - alloc` |
| `request[N][M]` | jo maanga hai par mila nahi (pending) |

**Golden rule (invariant):** `Available + sum(Allocation) = Total`. Har grant
aur release ke baad program isko check karta hai.

## 5. Banker's Safety Algorithm (`safety_check` in `banker.c`)

1. `Work = Available`, sab `Finish = false`.
2. Aisa node dhundo jiska `Need <= Work`.
3. Woh finish ho sakta hai, to `Work = Work + Allocation`, `Finish = true`.
4. Step 2 dobara. Sab finish ho gaye to **SAFE**.

### Haath se calculation (TC1, board pe yahi karo)

Resources (A B C), Available = 3 3 2

| Node | Max | Alloc | Need |
|---|---|---|---|
| N0 | 7 5 3 | 0 1 0 | 7 4 3 |
| N1 | 3 2 2 | 2 0 0 | 1 2 2 |
| N2 | 9 0 2 | 3 0 2 | 6 0 0 |
| N3 | 2 2 2 | 2 1 1 | 0 1 1 |
| N4 | 4 3 3 | 0 0 2 | 4 3 1 |

| Step | Node | Need <= Work? | Naya Work |
|---|---|---|---|
| 1 | N0 | 743 <= 332? Nahi | 3 3 2 |
| 2 | N1 | 122 <= 332? Haan | 5 3 2 |
| 3 | N2 | 600 <= 532? Nahi | 5 3 2 |
| 4 | N3 | 011 <= 532? Haan | 7 4 3 |
| 5 | N4 | 431 <= 743? Haan | 7 4 5 |
| 6 | N0 | 743 <= 745? Haan | 7 5 5 |
| 7 | N2 | 600 <= 755? Haan | 10 5 7 |

Safe sequence: **<N1, N3, N4, N0, N2>**. Aakhri Work = Total, yeh answer ka
cross-check hai.

### Resource-Request Algorithm (`try_allocate`)

1. `Request <= Need`? Nahi to **error**.
2. `Request <= Available`? Nahi to **wait**.
3. **Jhooth-mooth allocate** karo, Safety chalao. Unsafe ho to **rollback**
   aur wait.

* TC2: N1 maangta hai (1,0,2): state safe rehti hai, **GRANTED**.
* TC3: N0 maangta hai (0,2,0): resources free hain, phir bhi **DENIED**, kyunki
  Available (2,1,0) reh jayega aur kisi ki bhi Need poori nahi ho sakti.

> **Important line:** Unsafe ka matlab deadlock nahi hai. Matlab hai ki deadlock
> *ho sakta hai* aur system guarantee nahi de sakta.

## 6. Deadlock Detection (`detector.c`)

### Wait-For Graph + DFS
Edge `Ni → Nj`: Ni aisa resource maang raha hai jo free nahi hai aur Nj ke paas
hai. DFS mein 3 colour: 0 = visit nahi hua, 1 = current path pe, 2 = ho gaya.
Colour 1 wala node dobara mile to **back edge**, yaani **cycle**.

* Single instance resources: **cycle = deadlock** (pakka).
* Multi instance: cycle zaroori hai par **kaafi nahi**. Demo step 3 mein cycle
  dikhta hai par deadlock nahi hai. Yeh teacher ko zaroor dikhana.

### Multi-instance algorithm (`detect_deadlock`)
Safety jaisa hi hai, bas 2 farak:

| | Safety (Banker) | Detection |
|---|---|---|
| Kya compare hota hai | `Need` (future ka max) | `Request` (abhi ki maang) |
| Shuru mein Finish true | kisi ka nahi | jiske paas kuch nahi |
| Sawal | "aage safe hai?" | "abhi koi phansa hai?" |

Jiska `Finish` false reh gaya, woh deadlocked hai.

**TC6 ka order:** program `<N0,N2,N3,N4,N1>` print karta hai, synopsis mein
`<N0,N2,N3,N1,N4>` hai. Dono sahi hain, kyunki N3 ke baad N1 aur N4 dono chal
sakte hain. Program index order mein aage badhta hai, isliye N4 pehle aata hai.

**Complexity:** dono algorithms worst case mein O(M × N²).

## 7. Recovery (`recovery.c`)

```
cost = 10*priority + 3*resources_held + 1*work_done + 50*pehle_kitni_baar_victim_bana
```

Deadlocked nodes mein se **sabse kam cost** wala victim banta hai. Uske
resources wapas le liye jate hain. Phir detection dobara chalta hai, jab tak
deadlock khatam na ho.

* **PREEMPT:** node apne job ki shuruaat (checkpoint) par rollback karta hai,
  thoda ruk kar dobara try karta hai.
* **TERMINATE:** node ka thread band ho jata hai.
* **50 ka penalty kyun?** Taaki har baar ek hi node victim na bane (starvation).

## 8. Threads aur Synchronization (`sync.c`, `simulator.c`)

| Cheez | Kahan use hui | Kyun |
|---|---|---|
| `pthread_create` | har node ek thread + detector thread | nodes saath-saath chalte hain |
| `pthread_mutex_t lock` | saari matrices ke upar | race condition na ho |
| `pthread_cond_t changed` | blocked node isme sota hai | busy waiting nahi |
| `pthread_cond_broadcast` | grant ya recovery ke baad | sab jaag kar apna status check karein |
| `sem_t start_gate` | counting semaphore | saare node threads ek saath start hon |
| `sem_t detector_wakeup` | `sem_timedwait` | detector har T ms jaage, ya stop karte time turant |

**Monitor kya hai?** Shared data + ek lock + condition variable. Data ko sirf
lock lekar chhedte hain.

**`pthread_cond_wait` while loop mein kyun?** Spurious wake-up ho sakta hai,
aur broadcast sabko jagata hai. Jaagne ke baad condition dobara check karni
padti hai. `wait` karte waqt mutex apne aap chhoot jata hai.

**FIFO queue:** purani request pehle check hoti hai. Agar woh grant nahi ho
sakti to peeche wali check hoti hai, kyunki ho sakta hai pehli wali usi ka
resource maang rahi ho. Koi node 25 baar overtake ho jaye to jinke paas kuch
nahi hai unko rok dete hain (starvation protection).

## 9. Ek request ka safar

```
node thread -> monitor_request()
   -> mutex lock
   -> validate (Request <= Need, <= Total)
   -> queue mein daalo -> monitor_process_queue_locked()
        -> try_allocate()   [avoidance mein Safety bhi]
   -> mil gaya? return
   -> nahi mila? pthread_cond_wait (so jao)
        <- release / recovery ke baad broadcast
   -> mutex unlock
```

## 10. Live demo (lagbhag 5 minute)

Linux/WSL: `make demo`. Windows: `.\run.bat demo`. Har step ke baad Enter.

| Step | Kya dikhega | Kya bolna hai |
|---|---|---|
| 1 | safe sequence | "Banker pehle hi bata deta hai ki sab kis order mein finish honge" |
| 2 | cycle N1→N2→N3→N1 | "single instance mein cycle matlab deadlock" |
| 3 | cycle hai par NO DEADLOCK | "multi instance mein sirf cycle kaafi nahi" |
| 4 | deadlock, victim, recovery | "sabse sasta victim, uske baad baaki finish ho sakte hain" |
| 5 | threads sach mein phanste hain | "detector thread ne pakda aur recover kiya" |
| 6 | wahi script, koi deadlock nahi | "Banker ne unsafe request rok di" |
| 7 | comparison table | neeche dekho |

Menu mein haath se TC2, TC3, TC4: option 1 → `config/textbook.txt`, phir
option 5:

1. `N1`, `1 0 2` → GRANTED
2. `N0`, `0 2 0` → DENIED (unsafe)
3. `N3`, `0 3 0` → ERROR: request exceeds need

### Comparison kaise samjhayein
* Avoidance: deadlock **0**, par Banker kai requests rok deta hai.
* Detection: requests zyada aasani se milti hain, par deadlock hote hain aur
  rollback mein kaam **dobara** karna padta hai.
* Isliye `grants/s` akela dekhna galat hai, usme dobara kiya kaam bhi gina jata
  hai. **Completed nodes per second** bhi dekho.
* Numbers har run mein badalte hain (thread scheduling). **Koi number ratna
  mat**, jo screen pe aaye wahi samjhana. "Kaun sa mode hamesha tez hai" aisa
  claim mat karna.

## 11. Viva ke sawal aur jawab

**Q. Process ki jagah thread kyun?**
Threads same memory share karte hain, to resource tables share karna aasan hai.
Isi wajah se synchronization zaroori ho jata hai.

**Q. Mutex aur semaphore mein farak?**
Mutex ka owner hota hai, jo lock kare wahi unlock kare. Semaphore ek counter hai,
`wait` ghatata hai, `post` badhata hai, aur koi bhi thread post kar sakta hai.

**Q. Race condition kaise roki?**
Saari shared matrices ek hi mutex ke andar change hoti hain. Aur har change ke
baad invariant check hota hai.

**Q. Kya aapka monitor khud deadlock ho sakta hai?**
Sirf ek mutex hai, to do locks ka circular wait ban hi nahi sakta. Aur
`pthread_cond_wait` sote waqt mutex chhod deta hai.

**Q. Banker's algorithm ki limitations?**
Max claim pehle se pata hona chahiye, nodes aur resources fixed hain, aur har
request pe safety check ka kharcha hai.

**Q. Detector kitni der mein chalta hai?**
Configurable hai (`--interval`, default 10 ms). Chhota interval: jaldi pakdega,
zyada CPU. Bada interval: deadlock der tak phansa rahega.

**Q. Avoidance mode mein detector kyun nahi?**
Banker sirf safe states banne deta hai, aur safe state mein deadlock nahi hota.

**Q. Testing kaise ki?**
`make test`: synopsis ke TC1 se TC8, aur recovery, invalid input, speed ke
extra checks. TC8 mein 20 threads, har ek ke 50 operations.

**Q. Helgrind chalaya?**
`make helgrind` Linux par chalta hai. **Jab tak khud chala kar na dekho, "haan,
clean hai" mat bolna.** (Synopsis NFR1 mein Helgrind likha hai, isliye
presentation se pehle Ubuntu par chala lo.)

## 12. Team mein kaun kya samjhaye (synopsis ke hisaab se)

| Member | Modules | Files | Guide sections |
|---|---|---|---|
| Ishita | M1, M2 | `topology.c`, `simulator.c` | 4, 8 |
| Priyani | M3, M4 | `banker.c`, `sync.c` | 5, 8, 9 |
| Kartik | M5, M6 | `detector.c`, `recovery.c`, `logger.c` | 6, 7, 10 |

Sabko sections 1, 2, 3 aur 11 aane chahiye.

**Padhne ka order:** `nrade.h` → `banker.c` → `sync.c` → `detector.c` →
`recovery.c` → `simulator.c`. `main.c` aur `logger.c` zyada tar input/output hai.
