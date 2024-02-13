#!/bin/sh

## step1: install pip
apt update
apt install -y pip

## Hint: If permission is denied.
# sudo apt update
# sudo apt install -y pip

## step2: install python packages
pip install setuptools
pip install scikit-build
pip install qiskit
pip install matplotlib
pip install pybind11
pip install cmake
pip install virtualenv

## step3: download pybind11
if [ ! -d "pybind11" ]; then
	git clone https://github.com/pybind/pybind11.git
fi

## step4: install qiskit wrapper for QPlayer
pip install .

# /** START UPDATE[01.16] */
## step5: copy file 'qelib1.inc' to a folder in your python package.
# Get the path of the 'site-packages' folder for the default Python version
site_packages_path=$(pip show qplayer | grep -i "location" | awk '{print $2}')

# Check if '[site-package_path]/qplayer' exists.
if [ ! -e "$site_packages_path/qplayer" ]; then
	echo "Error: '$site_packages_path/qplayer' not found."
	exit 1
fi

# Check if 'qelib1.inc' exists in [site-packages_path]
if [ -f "$site_packages_path/qplayer/qelib1.inc" ]; then
	echo "'qelib1.inc' already exists in [$site_packages_path]."
	exit 0
fi


# Find the location of 'qelib1.inc' in the project
qelib_path=$(find ../../ -name qelib1.inc -print -quit)		# Note: Get the path to the first qelib1.inc file in the search results.

# Check if 'qelib1.inc' is found
if [ -z "$qelib_path" ]; then
	echo "Error: 'qelib1.inc' not found in the project."
	exit 1
fi


# Copy qelib1.inc to [site_packages_path]
cp "$qelib_path" "$site_packages_path/qplayer/"

# Check for copy success
if [ $? -eq 0 ]; then
	echo "'qelib1.inc' copied successfully from [$qelib_path] to [$site_packages_path]."
else
	echo "Error: Faild to copy 'qelib1.inc'."
	echo "   Target path: [$site_packages_path/qplayer]"
fi
# /** END of UPDATE[01.16] */