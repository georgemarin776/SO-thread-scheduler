Soluția propusă folosește urmatoarea structură pentru thread-uri:
    - tid, 
    - priority (util pentru deciderea următorului thread),
    - time (valoare ce reprezintă timpul de execuție rămas),
    - status (implementat cu o enumerare),
    - device (pentru so_signal, funcție a cărei implementare nu este completă),
    - handler,
    - thread_sem (semnal).


De menționat din rândul câmpurilor scheduler-ului ar fi priority queue-ul:
am implementat o coadă de priorități cu vector (cu funcțiile aferente,
pop și push).


Funcțiile so_init, so_end, so_exec, so_wait, so_fork au fost implementate
conform cerinței, iar so_signal este incompletă. Am preferat utilizarea
mai multor funcții ajutătoare pentru a ușura citirea codului.

Descrierea pentru cele 5 funcții so menționate (fără so_signal) se află
în comentariile din "so_scheduler.c".