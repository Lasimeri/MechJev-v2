# MechJev-v2. `make` builds the decision engine; `make check` runs the
# checks that need no model; `make serve` / `make stop` run the engine.

LLAMA_CPP_DIR ?= $(HOME)/llama.cpp
LLAMA_BUILD   ?= $(LLAMA_CPP_DIR)/build/bin
CC            ?= gcc
CFLAGS        ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -pthread
B             := backend/build
SRC           := backend/src

ENGINE_OBJS := $(B)/util.o $(B)/json.o $(B)/prompt.o $(B)/engine.o $(B)/judge.o \
               $(B)/http.o $(B)/conf.o $(B)/main.o

.PHONY: all help build check serve stop talk stats clean

all: build

help: ## Show this help
	@grep -E '^[a-zA-Z0-9_-]+:.*?## .*$$' $(MAKEFILE_LIST) | awk 'BEGIN {FS = ":.*?## "}; {printf "  %-10s %s\n", $$1, $$2}'

build: $(B)/xks $(B)/learner ## Build the decision engine (xks, against llama.cpp's CUDA build) and the learner (no llama.cpp)

$(B):
	@mkdir -p $(B)

$(B)/engine.o: $(SRC)/engine.c $(SRC)/*.h | $(B)
	$(CC) $(CFLAGS) -I$(LLAMA_CPP_DIR)/include -I$(LLAMA_CPP_DIR)/ggml/include -c $< -o $@

$(B)/%.o: $(SRC)/%.c $(SRC)/*.h | $(B)
	$(CC) $(CFLAGS) -c $< -o $@

$(B)/xks: $(ENGINE_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -L$(LLAMA_BUILD) -lllama -lggml -lggml-base -Wl,-rpath,$(LLAMA_BUILD) -lm

$(B)/json_roundtrip: backend/tests/json_roundtrip.c $(B)/json.o $(B)/util.o
	$(CC) $(CFLAGS) -o $@ $^ -lm

check: $(B)/json_roundtrip $(B)/learner_test ## Checks that need no model: JSON against serde_json, the learner store
	@$(B)/json_roundtrip < backend/tests/json_cases.jsonl | cmp - backend/tests/json_cases.serde.out && echo "json: identical to serde_json on backend/tests/json_cases.jsonl"
	@$(B)/learner_test

serve: build ## Start the engine detached on XKS_BIND (loads the subject)
	$(B)/xks serve --detach

talk: build ## Talk with the learner on this terminal (the engine must be up: make serve)
	backend/build/learner talk

stats: build ## What the learner knows
	backend/build/learner stats

stop: ## Stop a detached engine
	$(B)/xks stop

clean:
	rm -rf $(B)

# The learner links no llama.cpp: its decisions come from xks over HTTP.
LEARNER_OBJS := $(B)/util.o $(B)/json.o $(B)/http.o $(B)/conf.o $(B)/circuit.o $(B)/learner.o \
                $(B)/speak.o $(B)/learner_main.o

$(B)/learner: $(LEARNER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

$(B)/learner_test: backend/tests/learner_test.c $(B)/learner.o $(B)/circuit.o $(B)/util.o
	$(CC) $(CFLAGS) -o $@ $^ -lm
