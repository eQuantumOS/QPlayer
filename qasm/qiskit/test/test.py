from qiskit import QuantumCircuit
from qiskit.visualization import plot_histogram
from qplayer import executeQASM
import matplotlib.pyplot as plt

qc = QuantumCircuit(2)
qc.h(0)
qc.h(1)
qc.y(0)
qc.cx(0, 1)
qc.measure_all()

result = executeQASM(qc, 16)
figure = plot_histogram(result.get_counts(), figsize=(10, 6))

#case1: store result image to file
#figure.savefig('result.png')

#case2: display result image in terminlal
plt.show();
