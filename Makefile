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

.PHONY: all help build frontend check serve stop talk stats tui watch mcp clean

all: build

help: ## Show this help
	@grep -E '^[a-zA-Z0-9_-]+:.*?## .*$$' $(MAKEFILE_LIST) | awk 'BEGIN {FS = ":.*?## "}; {printf "  %-10s %s\n", $$1, $$2}'

build: $(B)/xks $(B)/learner frontend ## Build the decision engine (xks, against llama.cpp's CUDA build), the learner (no llama.cpp) and the front end (mjev2)

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

check: $(B)/json_roundtrip $(B)/learner_test $(B)/learner_test_asan ## Checks that need no model: JSON against serde_json, the learner, the front end's tests
	@$(B)/json_roundtrip < backend/tests/json_cases.jsonl | cmp - backend/tests/json_cases.serde.out && echo "json: identical to serde_json on backend/tests/json_cases.jsonl"
	@$(B)/learner_test
	@ASAN_OPTIONS=detect_leaks=0 $(B)/learner_test_asan | tail -1 | sed 's/^/asan: /'
	@cd frontend && cargo test --release --offline -q 2>&1 | grep -E "test result" | head -1

serve: build ## Start the engine detached on XKS_BIND (loads the subject)
	$(B)/xks serve --detach

talk: build ## Talk with the learner on this terminal (the engine must be up: make serve)
	backend/build/learner talk

stats: build ## What the learner knows
	backend/build/learner stats

stop: ## Stop a detached engine
	$(B)/xks stop

clean:
	rm -rf $(B) frontend/target

# The learner links no llama.cpp: its decisions come from xks over HTTP.
LEARNER_OBJS := $(B)/util.o $(B)/json.o $(B)/http.o $(B)/conf.o $(B)/circuit.o $(B)/learner.o \
                $(B)/speak.o $(B)/learner_main.o

$(B)/learner: $(LEARNER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lm

$(B)/learner_test: backend/tests/learner_test.c $(B)/learner.o $(B)/circuit.o $(B)/util.o
	$(CC) $(CFLAGS) -o $@ $^ -lm

frontend: ## Build the front end, frontend/target/release/mjev2 (Rust, offline)
	cd frontend && cargo build --release --offline

tui: frontend ## The TUI on this terminal (the talk screen: t)
	frontend/target/release/mjev2

watch: frontend ## The TUI in tmux session jev2, opened in a Konsole window (what mjev2 mcp drives)
	@tmux has-session -t jev2 2>/dev/null || tmux new-session -d -s jev2 -x 200 -y 50 "$(CURDIR)/frontend/target/release/mjev2"
	@setsid konsole --title MechJev-v2 -e tmux attach -t jev2 >/dev/null 2>&1 &

mcp: frontend ## The MCP server on stdio (for claude mcp add)
	frontend/target/release/mjev2 mcp

# The learner test again under AddressSanitizer and UBSan: a stale write
# into the lexicon (fixed 2026-10-17) corrupted the live learner's heap.
$(B)/learner_test_asan: backend/tests/learner_test.c $(SRC)/learner.c $(SRC)/circuit.c $(SRC)/util.c $(SRC)/*.h | $(B)
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -std=gnu11 -pthread -o $@ backend/tests/learner_test.c $(SRC)/learner.c $(SRC)/circuit.c $(SRC)/util.c -lm
