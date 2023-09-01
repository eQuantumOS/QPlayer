# QPlayer v2.0-Puma 
### Light-weight, Scalable and Fast Quantum Simulator
  
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

# Publication
 - QPlayer: Lightweight, scalable, and fast quantum simulator, 2022, [ETRI Journal (SCIE, IF1.4)](https://doi.org/10.4218/etrij.2021-0442)
 - Multilayered logical qubits and synthesized quantum bits, 2023, [Quantum Science and Technology (SCIE, IF6.7)](https://doi.org/10.1088/2058-9565/accec5)

# OS environment
 - CentOS 7.x or higher
 - Ubuntu 20.x or higher

# Installation
<pre>
 (1) install boost library
     ubuntu> apt-get install libboost-all-dev
     linux> yum install -y boost-devel

 (2) build qplayer
     >> make
</pre>

# Supporting Quantum Operations
<pre>
 - initZ              : initialize qubit to |0>
 - initX              : initialize qubit to |+>
 - I                  : pauli identity gate
 - X                  : pauli X gate
 - Z                  : pauli Z gate
 - Y                  : pauli Y gate
 - Hadamard           : hadamard gate
 - RX                 : rotate X-axis gate
 - RY                 : rotate Y-axis gate
 - RZ                 : rotate Z-axis gate
 - S                  : S gate
 - T                  : T gate
 - S+                 : S dagger
 - T+                 : S dagger
 - P                  : phase gate
 - SX                 : square-root x gate
 - U1                 : single-qubit rotation gate about the Z axis
 - U2                 : single-qubit rotation gate about the X+Z axis
 - U3                 : single-qubit rotation gate with 3 euler angles
 - SWAP               : SWAP gate
 - CX                 : controlled x gate
 - CY                 : controlled y gate
 - CZ                 : controlled z gate
 - CH                 : controlled h gate
 - CCZ                : controlled rz gate
 - CCX                : controlled-controlled x gate
 - CU1                : controlled u1 gate
 - CU2                : controlled u2 gate
 - CU3                : controlled u3 gate
 - CSWAP              : controlled swap gate
 - measure(z basis)   : mesure qubit
</pre>

# QPlayer Source Tree
<pre>
  + core                : qplayer core library
  + docs                : manual and documentations
  + qasm                : OpenQASM 2.0 parser & executor
  + test - general      : test examples for general quantum algorithms
         - surfacecode  : test examples for surface code
         - MLQ          : experimental code to support 20 logical qubits with d-3 surface code
         - verification : verification code to validate QPlayer installation
         - benchmark    : several benchmark codes
</pre>
