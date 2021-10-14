# QPlayer v0.2 : Light-weight, Scalable and Fast Quantum Simulator

With the rapid evolution of quantum computing, classical quantum simulations
are considered essential for quantum algorithm verification, quantum error
analysis, and development of new quantum applications. However, the exponential
increase in memory overhead and operation time are challenging issues that
have not been solved for many years. We provide the QPlayer that provides
more qubits and faster quantum operations with smaller memory than before.


# Depencencies
 - gcc and g++ capable of C++11 standard
 - make 3.x or higher
 - boost library 1.61 or higher
   (only if you want to support more than 64 qubits)

# OS environment
 - CentOS 7.x or higher
 - Ubuntu 15.x or higher

# Installation
 (1) 1~64 qubits
     - make

 (2) 65~1024 qubits (you must install boost library)
     - make BOOST=yes

# Supporting Quantum Operations
 - initZ              : initialize qubit to |0>
 - initX              : initialize qubit to |+>
 - pauliI             : pauli identity gate
 - pauliX             : pauli X gate
 - pauliZ             : pauli Z gate
 - pauliY             : pauli Y gate
 - hadamard           : hadamard gate
 - cx                 : controlled NOT gate
 - cz                 : controlled Z gate
 - ccx                : controlled-controlled NOT gate
 - rotate-x           : rotate X-axis gate
 - rotate-y           : rotate Y-axis gate
 - rotate-z           : rotate Z-axis gate
 - S                  : S gate
 - T                  : T gate
 - S+                 : S dagger
 - T+                 : S dagger
 - U1                 : Universal gate
 - U2                 : Universal gate
 - U3                 : Universal gate
 - swap               : SWAP gate
 - measure(z basis)   : mesure qubit

# QPlayer Source Tree
  + qplayer                 : qplayer sources
  + circuit -+ basic.cpp    : 'hello world' for qplayer
             + qft.cpp      : quantum fourier transform
             + grover.cpp   : quantum search algorithm

