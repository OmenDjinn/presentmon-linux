# End-to-end tests: Python 3 stdlib drivers that run vk_truth under the build-tree layer next to the presentmon CLI
# on the real desktop (Wayland, GPU).  One ctest entry per module.  All of them are labelled "live" and take the
# "desktop" resource lock, because they open windows and measure frame timing.  They skip (ctest "skipped", exit code 77 from runtests.py) when there is no Wayland session or the displays are asleep.
#
#   ctest --test-dir <build> -L e2e --output-on-failure        # whole suite, about 4-5 minutes
#   PML_DESKTOP_LOCK=/path/to/lockfile  makes every scenario hold an flock while it runs (shared desktops)
find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_FOUND)
	message(STATUS "tests/e2e: python3 not found, end-to-end tests not registered")
	return()
endif()

set(PML_E2E_DIR ${CMAKE_CURRENT_SOURCE_DIR})

function(pml_add_e2e mod timeout)
	add_test(NAME e2e.${mod}
	         COMMAND ${Python3_EXECUTABLE} runtests.py test_${mod}
	         WORKING_DIRECTORY ${PML_E2E_DIR})
	set_tests_properties(e2e.${mod} PROPERTIES
		LABELS "e2e;live"
		RESOURCE_LOCK desktop
		TIMEOUT ${timeout}
		ENVIRONMENT "PML_BUILD_DIR=${CMAKE_BINARY_DIR};PYTHONDONTWRITEBYTECODE=1;PYTHONUNBUFFERED=1"
		SKIP_RETURN_CODE 77)
endfunction()

# module (tests/e2e/test_<module>.py), timeout in seconds
pml_add_e2e(csv_truth    240)
pml_add_e2e(fullscreen   120)
pml_add_e2e(load         120)
pml_add_e2e(dxvk_share   240)
pml_add_e2e(xwayland     120)
pml_add_e2e(lifecycle    180)
pml_add_e2e(multiprocess 120)
pml_add_e2e(mangohud      90)
pml_add_e2e(overhead     240)
pml_add_e2e(cap          900)   # frame caps; the comparisons skip on a shared desktop (PML_E2E_SHARED_DESKTOP=1)
pml_add_e2e(proton       300)   # skips without a GE-Proton (DXVK 3.x) or without bin/d3d11_truth.exe
