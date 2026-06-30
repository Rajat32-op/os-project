import os
import json
import time
import random
import fcntl

# Simple Q-learning online bandit parameters
EPSILON = 0
EPSILON_DECAY = 0.99995
MIN_EPSILON = 0.05
ALPHA = 0.1
GAMMA = 0.8 # since state space is mostly stateless bandit context right now

# Discrete Actions we can send back
ACTIONS = [
    "DO_NOTHING",   # Don't intervene
    "PERF_100",     # High freq, performance gov
    "POWERSAVE_50", # Low freq, powersave gov, E-cores
    "SWAP_80",      # Memory bound, tuning swappiness
    "IO_BFQ"        # IO bound, bfq scheduler
]

# We discretize the continuous state into bins to keep Q-table small.
# Normalize and bin: e.g. util, ipc, mpki, iowait, disk_io
def discretize_state(data):
    def get_bin(val, max_val=100.0, num_bins=3):
        norm = max(0.0, min(1.0, float(val) / max_val))
        # 3 bins: 0, 1, 2
        if norm > 0.6: return 2
        if norm > 0.2: return 1
        return 0

    b_u = get_bin(data.get('util', 0), 100.0)
    b_ipc = get_bin(data.get('ipc', 0), 3.0) 
    b_mpki = get_bin(data.get('mpki', 0), 50.0)
    b_io = get_bin(data.get('iowait', 0), 100.0) 
    b_power=get_bin(data.get('power',0),30.0)
    b_freq = get_bin(data.get('mhz',0),3500.0)
    
    return f"{b_u}_{b_ipc}_{b_mpki}_{b_io}_{b_power}_{b_freq}"

class RLAgent:
    def __init__(self, state_fifo="/tmp/state_fifo", action_fifo="/tmp/action_fifo"):
        self.state_fifo_path = state_fifo
        self.action_fifo_path = action_fifo
        self.q_table = {}
        self.reward_buffer = []
        self.REWARD_WINDOW = 3
        self.epsilon = EPSILON
        
        self.last_state = None
        self.last_action = None
        
        self.model_file = "q_table.json"
        self.last_save_time = time.time()
        
        # Load saved model if exists
        if os.path.exists(self.model_file):
            print(f"[RL Agent] Loading saved model from {self.model_file}")
            try:
                with open(self.model_file, 'r') as f:
                    self.q_table = json.load(f)
            except Exception as e:
                print(f"[RL Agent] Failed to load model: {e}")
        
        # Ensure FIFOs exist
        if not os.path.exists(self.state_fifo_path):
            os.mkfifo(self.state_fifo_path)
        if not os.path.exists(self.action_fifo_path):
            os.mkfifo(self.action_fifo_path)

    def run(self):
        print("[RL Agent] Starting online learning loop...")
        
        # Open FIFO for reading state
        try:
            state_fd = os.open(self.state_fifo_path, os.O_RDONLY | os.O_NONBLOCK)
        except Exception as e:
            print(f"[RL Agent] Error opening state fifo: {e}")
            return

        with open(self.action_fifo_path, 'w') as action_fifo:
            buffer = ""
            while True:
                try:
                    chunk = os.read(state_fd, 1024).decode('utf-8')
                    if chunk:
                        buffer += chunk
                        while '\n' in buffer:
                            line, buffer = buffer.split('\n', 1)
                            if line.strip():
                                self.process_state(line, action_fifo)
                    else:
                        time.sleep(0.1)
                except BlockingIOError:
                    time.sleep(0.1)
                except Exception as e:
                    print(f"[RL Agent] Read error: {e}")
                    time.sleep(0.1)

    def process_state(self, json_str, action_fifo):
        try:
            data = json.loads(json_str)
        except json.JSONDecodeError:
            return

        raw_reward = data.get("reward", 0.0)

        self.reward_buffer.append(raw_reward)
        if len(self.reward_buffer) > self.REWARD_WINDOW:
            self.reward_buffer.pop(0)

        reward = sum(self.reward_buffer) / len(self.reward_buffer)
        state_key = discretize_state(data)
        
        # Initialize state in Q-table if not present
        if state_key not in self.q_table:            
            self.q_table[state_key] = {
                a: 0.0 for a in ACTIONS
            }
        
        # Q-Learning update
        if self.last_state and self.last_action:
            old_q = self.q_table[self.last_state][self.last_action]
            max_future_q = max(self.q_table[state_key].values())
            # Update formula
            new_q = old_q + ALPHA * (reward + GAMMA * max_future_q - old_q)
            self.q_table[self.last_state][self.last_action] = new_q
            
        # Decay Epsilon
        self.epsilon = max(MIN_EPSILON, self.epsilon * EPSILON_DECAY)
        
        # Choose next action (Epsilon Greedy)
        if random.random() < self.epsilon:
            action = random.choice(ACTIONS)
        else:
            action = max(self.q_table[state_key], key=self.q_table[state_key].get)
            
        prev_action = self.last_action

        if prev_action and action != prev_action:
            reward -= 0.05

        self.last_state = state_key
        self.last_action = action
                # Log
        print(f"[RL] S:{state_key} R:{reward:.2f} eps:{self.epsilon:.3f} -> A:{action}")
        
        # Save model every 5 seconds
        current_time = time.time()
        if current_time - self.last_save_time >= 5.0:
            try:
                with open(self.model_file, 'w') as f:
                    json.dump(self.q_table, f)
                self.last_save_time = current_time
            except Exception as e:
                print(f"[RL Agent] Error saving model: {e}")
        
        # Send action to C++
        out_msg = json.dumps({"action": action, "confidence": 1.0 - self.epsilon}) + "\n"
        try:
            action_fifo.write(out_msg)
            action_fifo.flush()
        except BrokenPipeError:
            pass

if __name__ == "__main__":
    agent = RLAgent()
    agent.run()
