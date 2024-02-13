/*
#######################################################################
#   core.cpp: QPlayer wrapper core file
#   
#   Created: 2023. 12. 02
#   Last Modified: 2023. 01. 16
#
#   Authors:
#      Nagyeong Choi (choi2019@pukyong.ac.kr)
#      Sengthai Heng (sengthai@pukyong.ac.kr)
#
#   Quantum Computing Laboratory, PKNU (quantum.pknu.ac.kr)
#######################################################################
*/

#include <pybind11/pybind11.h>
#include <filesystem>
#include <cstdlib>

#include "Parser.h"

#define MAX_LENG 512


/** START UPDATE[01.16] */
#include <iostream>
#include <libgen.h>
#include <dlfcn.h>

/** 
 * 	@brief Returns the library location of the currently executing code.
 * 
 *  @return The path to the library directory.
*/
std::string getLibraryLocation() {
    Dl_info info;
    if (dladdr((void*)getLibraryLocation, &info)) {
		std::string fullPath(info.dli_fname);
		return std::string(dirname(&fullPath[0]));
    }
	return "";
}
// std::string libraryLocation(getLibraryLocation());
/** END of UPDATE[01.16] */

/**
 * @brief Converts a gate value to a string representation.
 * 
 * @param gate The gate value to convert.
 * @return The string representation of the gate value.
 */
std::string convertGate2String(int gate) 
{
	switch(gate) {
	case QGATE_ID: return "ID";
	case QGATE_U1: return "U1";
	case QGATE_U2: return "U2";
	case QGATE_U3: return "U3";
	case QGATE_X: return "Pauli-X";
	case QGATE_Y: return "Pauli-Y";
	case QGATE_Z: return "Pauli-Z";
	case QGATE_H: return "H";
	case QGATE_S: return "S";
	case QGATE_P: return "P";
	case QGATE_SX: return "SX";
	case QGATE_SDG: return "SDG";
	case QGATE_T: return "T";
	case QGATE_TDG: return "TDG";
	case QGATE_RX: return "RX";
	case QGATE_RY: return "RY";
	case QGATE_RZ: return "RZ";
	case QGATE_CX: return "CX";
	case QGATE_CZ: return "CZ";
	case QGATE_CY: return "CY";
	case QGATE_CH: return "CH";
	case QGATE_CCX: return "CCX";
	case QGATE_CRZ: return "CRZ";
	case QGATE_CU1: return "CU1";
	case QGATE_CU2: return "CU2";
	case QGATE_CU3: return "CU3";
	case QGATE_SWAP: return "SWAP";
	case QGATE_CSWAP: return "CSWAP";
	case QGATE_MEASURE: return "MEASURE";
	}

	return "UNKNOWN GATE";
}

/**
 * @brief Saves a string to a temporary file.
 *
 * @param data The string to be saved.
 * @return The path of the temporary file.
 */
std::string saveStringToTempFile(const std::string& data) {
	char tempFileName[] = "/tmp/tempfileXXXXXX"; // Adjust the path as needed

	// Create a temporary file using C-style functions
	int tempFileDescriptor = mkstemp(tempFileName);
	if (tempFileDescriptor == -1) {
		std::cerr << "Error creating temporary file." << std::endl;
		return "";
	}

	// Open the temporary file using C-style functions
	FILE* tempFilePointer = fdopen(tempFileDescriptor, "w");
	if (!tempFilePointer) {
		close(tempFileDescriptor);
		std::cerr << "Error opening temporary file." << std::endl;
		return "";
	}

	// Write the string to the temporary file using C-style functions
	fwrite(data.c_str(), sizeof(char), data.size(), tempFilePointer);

	fclose(tempFilePointer);

	return tempFileName;
}

/**
 * @brief Removes a file from the file system.
 * 
 * This function removes the file specified by the given file path.
 * If the file cannot be removed, an error message is printed to the standard error stream.
 * 
 * @param filePath The path of the file to be removed.
 */
void removeFile(const std::string& filePath) {
	if (std::remove(filePath.c_str()) != 0) {
		std::cerr << "Error removing file: " << filePath << std::endl;
	}
}

/**
 * @brief Replaces all occurrences of a substring in a given string with a replacement string.
 *
 * @param originalString The original string to be modified.
 * @param substringToReplace The substring to be replaced.
 * @param replacementString The string to replace the substring with.
 */
void replaceSubstring(std::string& originalString, const std::string& substringToReplace, const std::string& replacementString) {
	// Find the position of the substring in the original string
	size_t pos = originalString.find(substringToReplace);

	// Check if the substring was found
	if (pos != std::string::npos) {
		// Replace the substring with the new string
		originalString.replace(pos, substringToReplace.length(), replacementString);
	} else {
		std::cout << "Substring not found." << std::endl;
	}
}

/**
 * @brief Converts the given qregister_stat struct, cregMap, and shots into a JSON string.
 * 
 * @param stat The qregister_stat struct containing the quantum register statistics.
 * @param cregMap The map containing the classical register values.
 * @param shots The number of shots taken in the quantum computation.
 * @return The JSON string representation of the given data.
 */
std::string getStringJson(struct qregister_stat *stat,  map<std::string, int> &cregMap, int shots){
    
    char memTotalStr[32] = "";
    char memAvailStr[32] = "";
    char memUsedStr[32] = "";

    // convert char memUsedStr[32] to string type
    int pos = 0;

    human_readable_size(stat->memTotal, memTotalStr);
    human_readable_size(stat->memAvail, memAvailStr);
    human_readable_size(stat->memUsed, memUsedStr);

    std::string memUsedString(memUsedStr);
    std::string memAvailString(memAvailStr);
    std::string memTotalString(memTotalStr);
    
    std::string json = "{\n";

    json += " \"simulation_results\" : {\n";
    json += "  \"total_shots\" : " + to_string(shots) + ",\n";
    json += "  \"measured_states\" : " + to_string(cregMap.size()) + ",\n";

    if (cregMap.size() > 0) {
        json += "  \"measurement_details\" : [\n";
        for (auto entry : cregMap) {
            std::string percentage = to_string(entry.second * 100 / shots);
            json += "   {\n";
            json += "    \"percentage\" : " + percentage + ",\n";
            json += "    \"count\" : " + to_string(entry.second) + ",\n";
            json += "    \"state\" : \"" + entry.first + "\"\n";
            json += "   },\n";
        }
        // Remove the trailing comma and newline character
        json = json.substr(0, json.size() - 2) + "\n";
        json += "  ]\n";
    }

    json += " },\n";


    json += " \"circuit\" : {\n";
    json += "  \"used_qubits\" : " + to_string(stat->qubits) + ",\n";
    json += "  \"used_gates\" : " + to_string(stat->usedGates) + ",\n";
    json += "  \"gate_calls\" : {\n";

    pos = 0;
    for(int i=0; i<MAX_GATES; i++) {
        if(stat->gateCalls[i] != 0) {
            if(++pos < stat->usedGates) {
                json += "   \"" + convertGate2String(i) + "\" : " + to_string(stat->gateCalls[i]) + ",\n";
            } else {
                json += "   \"" + convertGate2String(i) + "\" : " + to_string(stat->gateCalls[i]) + "\n";
            }
        }
    }
    json += "  }\n";
    json += " },\n";

    json += " \"runtime\" : {\n";
    json += "  \"total_simulation_time\" : " + to_string(stat->tm_total) + ",\n";
    json += "  \"individual_gate_time\" : {\n";
    pos = 0;
    for(int i=0; i<MAX_GATES; i++) {
        if(stat->gateCalls[i] != 0) {
            if(++pos < stat->usedGates) {
                json += "   \"" + convertGate2String(i) + "\" : [" + to_string(stat->tm_gates_total[i]) + ", " + to_string(stat->tm_gates_max[i]) + ", " + to_string(stat->tm_gates_min[i]) + ", " + to_string(stat->tm_gates_avg[i]) + "],\n";
            } else {
                json += "   \"" + convertGate2String(i) + "\" : [" + to_string(stat->tm_gates_total[i]) + ", " + to_string(stat->tm_gates_max[i]) + ", " + to_string(stat->tm_gates_min[i]) + ", " + to_string(stat->tm_gates_avg[i]) + "]\n";
            }
        }
    }
    json += "  }\n";
    json += " },\n";

    json += " \"simulation_jobs\" : {\n";
    json += "  \"max_states\" : " + to_string((uint64_t)stat->maxQStates) + ",\n";
    json += "  \"final_states\" : " + to_string((uint64_t)stat->finalQStates) + ",\n";
    json += "  \"used_memory\" : \"" + memUsedString + "\"\n";
    json += " },\n";

    json += " \"system\" : {\n";
    json += "  \"os\" : {\n";
    json += "   \"name\" : \"" + string(stat->os_name) + "\",\n";
    json += "   \"version\" : \"" + string(stat->os_version) + "\"\n";
    json += "  },\n";

    json += "  \"cpu\" : {\n";
    json += "   \"model\" : \"" + string(stat->cpu_model) + "\",\n";
    json += "   \"cores\" : " + to_string(stat->cpu_cores) + ",\n";
    json += "   \"herz\" : \"" + string(stat->cpu_herz) + "\"\n";
    json += "  },\n";

    json += "  \"memory\" : {\n";
    json += "   \"total\" : \"" + memTotalString + "\",\n";
    json += "   \"avail\" : \"" + memAvailString + "\"\n";
    json += "  }\n";
    json += " }\n";

    json += "}\n";

    return json;
}

/**
 * @brief Executes the given QASM code and returns the result.
 * 
 * @param qasm The QASM code to execute.
 * @param shots The number of shots to perform (default: 1080).
 * @return The result of the execution.
 */
std::string executeQASM(std::string qasm, int shots=1080){
	map<string, int> cregMap;
	std::string json_str;
    
	// STEP 1 : preprocessing QASM file 
	
	/** STAERT UPDATE[01.16] */
	/* 
	//* Replace qelib1.inc path [1]
	//* : If you include qelib1.inc directly in the file.	
	// get current this file path 
	string currentPath = std::filesystem::path(__FILE__).parent_path().string();
	// Replace qelib1.inc path
	string qelib_path = currentPath + "/qelib1.inc";
	*/

	/*
	//* Replace qelib1.inc path [2]
	//* : If you specify the file path of qelib1.inc directly.
	// Replace qelib1.inc path
	string qelib_path = "/usr/local/lib/python3.10/dist-packages/qplayer/qelib1.inc";
	*/

	//* Replace qelib1.inc path [3]
	//* : Function to get the current library location.
	// Replace qelib1.inc path
	std::string qelib_path = getLibraryLocation() + "/qplayer/qelib1.inc";
	/** END of UPDATE[01.16] */

	replaceSubstring(qasm, "qelib1.inc", qelib_path);	

	std::string path = saveStringToTempFile(qasm);    

	// STEP 2 : execute QASM file for shot-round 
	Parser* parser = new Parser(path);
	parser->parse();

	for(int i=0; i<shots; i++) {
		std::string str;

		parser->run();
		parser->get_measure(str);

		auto it = cregMap.find(str);
		if(it == cregMap.end()) {
			cregMap[str] = 1;
		} else {
			it->second++;
		}

		parser->reset();
	}

	// STEP 4 : show simulation stat or generate json and result of measurement
	struct qregister_stat stat = parser->getQRegStat();

	if(cregMap.size() != 0) 
		json_str = getStringJson(&stat, cregMap, shots);

	// STEP 5 : delete path file and parser
	removeFile(path);

	delete parser;

	return json_str;
}

namespace py = pybind11;

/**
 * @brief Binds the module to Python using Pybind11.
 *
 * @param m The Pybind11 module object.
 */
PYBIND11_MODULE(qplayer_wra, m) {
    m.def("executeQASM", &executeQASM, "Description of executeQASM function",
      py::arg("_qasm"), py::arg("_shots")=1080);
}
