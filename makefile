project 			 := tiler # Pick name of project's executable here!

# Variables for path s of source and header files
inc_dir 			 := ./include
src_dir 			 := ./src
sources 			 := $(wildcard ${src_dir}/*.c)

# Variables for paths of object file and binary targets
build_dir   		 := ./build
obj_dir 			 := ${build_dir}/obj
bin_dir 			 := ${build_dir}/bin
executable 			 := ${bin_dir}/${project}
build_dirs 			 := ${obj_dir} ${bin_dir}
objects 			 := $(subst .c,.o,$(subst ${src_dir},${obj_dir},${sources}))

# C Compiler Configuration
CC      			 := gcc # Using gcc compiler (alternative: clang)
CFLAGS				 := -I${inc_dir} -g -Wall -Werror -ansi -pedantic-errors -O2
# CFLAGS options:
# -g 			Compile with debug symbols in binary files
# -Wall 		Warnings: all - display every single warning
# -Werror 		Warnings: treat warnings as errors
# -std=c89  	Use the 1989/1990 feature set
# -I${inc_dir}  Look in the include directory for include files
# -O2 			Disable compilation optimizations

linked_libs			:= -ldwmapi -luser32

# Phony rules do not create artifacts but are usefull workflow
.PHONY: all build debug clean

all: build

## Build the project by combining all object files
build: ${objects} | ${bin_dir}
	${CC} ${CFLAGS} -o ${executable} ${^} ${linked_libs}

## Build the project with debug flags
debug: ${objects} | ${bin_dir}
	${CC} ${CFLAGS} -o ${executable} ${^}


# Build object files from sources in a template pattern
${obj_dir}/%.o: ${src_dir}/%.c | ${obj_dir}
	${CC} ${CFLAGS} -c -o ${@} ${<}

# The build directories should be recreated when prerequisite
${build_dirs}:
	mkdir -p ${@}

# clean: Delete all artifacts produced in the build process
clean:
	rm -rf ${build_dir}
