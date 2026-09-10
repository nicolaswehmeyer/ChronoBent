# Set CHRONOBENT_DIR before including this file. No device bindings belong here.
CHRONOBENT_SOURCES := $(addprefix $(CHRONOBENT_DIR)/,$(shell cat $(CHRONOBENT_DIR)/sources.txt))
# Optional recorded-monophonic analysis; existing device consumers do not add it.
CHRONOBENT_TUNING_SOURCES := $(addprefix $(CHRONOBENT_DIR)/,$(shell cat $(CHRONOBENT_DIR)/tuning-sources.txt))
