# LoginWindow.app, gsdm's greeter (a GNUstep GUI application).
include $(GNUSTEP_MAKEFILES)/common.make

APP_NAME = LoginWindow
LoginWindow_OBJC_FILES = main.m GSDMController.m GSDMViews.m
LoginWindow_PRINCIPAL_CLASS = NSApplication

# A greeter, not something to start from /Applications: by default it goes
# with the domain's other system components. Override with
# GSDM_GREETER_INSTALL_DIR=<dir> on the make command line.
GSDM_GREETER_INSTALL_DIR ?= $(GNUSTEP_LIBRARY)/CoreServices
LoginWindow_INSTALL_DIR = $(GSDM_GREETER_INSTALL_DIR)

ADDITIONAL_OBJCFLAGS += -Wall

include $(GNUSTEP_MAKEFILES)/application.make
