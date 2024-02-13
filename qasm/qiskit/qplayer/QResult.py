#######################################################################
#   QResult.py 
#   
#   Created: 2023. 12. 13
#   Last Modified: 2023. 12. 14
#
#   Authors:
#      Sengthai Heng (sengthai@pukyong.ac.kr)
#      Nagyeong Choi (choi2019@pukyong.ac.kr)
#
#   Quantum Computing Laboratory, PKNU (quantum.pknu.ac.kr)
#######################################################################

from dataclasses import dataclass
from typing import List

@dataclass
class Circuit:
    used_qubits: int
    used_gates: int
    gate_calls: {}

    def __init__(self, used_qubits, used_gates, gate_calls):
        self.used_qubits = used_qubits
        self.used_gates = used_gates
        self.gate_calls = gate_calls


@dataclass
class Runtime:
    total_simulation_time: float
    individual_gate_time: {}

    def __init__(self, total_simulation_time, individual_gate_time):
        self.total_simulation_time = total_simulation_time
        self.individual_gate_time = individual_gate_time


@dataclass
class SimulationJobs:
    max_states: int
    final_states: int
    used_memory: str

    def __init__(self, max_states, final_states, used_memory):
        self.max_states = max_states
        self.final_states = final_states
        self.used_memory = used_memory


@dataclass
class MeasurementDetail:
    percentage: int
    count: int
    state: str

    def __init__(self, percentage, count, state):
        self.percentage = percentage
        self.count = count
        self.state = state


@dataclass
class CPU:
    model: str
    cores: int
    herz: str

    def __init__(self, model, cores, herz):
        self.model = model
        self.cores = cores
        self.herz = herz


@dataclass
class Memory:
    total: str
    avail: str

    def __init__(self, total, avail):
        self.total = total
        self.avail = avail


@dataclass
class OS:
    name: str
    version: str

    def __init__(self, name, version):
        self.name = name
        self.version = version


@dataclass
class System:
    os: OS
    cpu: CPU
    memory: Memory

    def __init__(self, os, cpu, memory):
        self.os = OS(**os)
        self.cpu = CPU(**cpu)
        self.memory = Memory(**memory)


@dataclass
class SimulationResults:
    total_shots: int
    measured_states: int
    measurement_details: List[MeasurementDetail]

    def __init__(self, total_shots, measured_states, measurement_details):
        self.total_shots = total_shots
        self.measured_states = measured_states
        self.measurement_details = [MeasurementDetail(**detail) for detail in measurement_details]


@dataclass
class QResult:
    simulation_results: SimulationResults
    circuit: Circuit
    runtime: Runtime
    simulation_jobs: SimulationJobs
    system: System
    
    def __init__(self, simulation_results, circuit, runtime, simulation_jobs, system):
        self.simulation_results = SimulationResults(**simulation_results)
        self.circuit = Circuit(**circuit)
        self.runtime = Runtime(**runtime)
        self.simulation_jobs = SimulationJobs(**simulation_jobs)
        self.system = System(**system)

    def get_counts(self):
        counts = {}
        for detail in self.simulation_results.measurement_details:
            counts[detail.state] = detail.count
        return counts

