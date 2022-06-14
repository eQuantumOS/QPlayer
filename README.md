# QPlayer v0.3 : Light-weight, Scalable and Fast Quantum Simulator

With the rapid evolution of quantum computing, classical quantum simulations
are considered essential for quantum algorithm verification, quantum error
analysis, and development of new quantum applications. However, the exponential
increase in memory overhead and operation time are challenging issues that
have not been solved for many years. We provide the QPlayer that provides
more qubits and faster quantum operations with smaller memory than before.

* fdaf
-fa
+fda

# Depencencies
 - gcc and g++ capable of C++11 standard
 - make 3.x or higher
 - boost library 1.61 or higher (only if you want to use more than 64 qubits)

# OS environment
 - CentOS 7.x or higher
 - Ubuntu 15.x or higher

# Installation
 ### using 1~64 qubits
<pre>
 (1) build qplayer  
    :> cd QPlayer  
    :> make  
</pre>

 ### using 65~1024 qubits 
<pre>
 (1) build boost library  
     ubuntu> apt-get install libboost-all-dev  
     linux> yum install -y boost-devel  
     
     --> If you want to install boost library manually, you should modify 
         QPlayer makefiles to include boost library path.
  
 (2) build qplayer  
    :> cd QPlayer  
    :> make boost=yes  
</pre>

# Supporting Quantum Operations
 - initZ              : initialize qubit to |0>
 - initX              : initialize qubit to |+>
 - I                  : pauli identity gate
 - X                  : pauli X gate
 - Z                  : pauli Z gate
 - Y                  : pauli Y gate
 - hadamard           : hadamard gate
 - rx                 : rotate X-axis gate
 - ry                 : rotate Y-axis gate
 - rz                 : rotate Z-axis gate
 - S                  : S gate
 - T                  : T gate
 - S+                 : S dagger
 - T+                 : S dagger
 - P                  : phase gate
 - u1                 : single-qubit rotation gate about the Z axis
 - u2                 : single-qubit rotation gate about the X+Z axis
 - u3                 : single-qubit rotation gate with 3 euler angles
 - swap               : SWAP gate
 - cx                 : controlled x gate
 - cy                 : controlled y gate
 - cz                 : controlled z gate
 - ch                 : controlled h gate
 - ccz                : controlled rz gate
 - ccx                : controlled-controlled x gate
 - cu1                : controlled u1 gate
 - cu2                : controlled u2 gate
 - cu3                : controlled u3 gate
 - measure(z basis)   : mesure qubit

# QPlayer Source Tree
<pre>
  + qplayer                     : qplayer sources  
  + circuit + basic.cpp         : 'hello world' for qplayer
            + qft.cpp           : quantum fourier transform
            + grover.cpp        : quantum search algorithm
            + RCS.cpp           : random circuit sampling
            + sc17_1LQ.cpp      : surface code 17 - 1 logical qubit operations
            + sc17_cnot.cpp     : surface code 17 - cnot with 3 logical qubits
            + sc17_hadamard.cpp : surface code 17 - hadamard 
</pre>
