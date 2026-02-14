class EnergyMonitor{
private:
    unsigned long long max_energy;
    unsigned long long prev_energy;
    unsigned long long cur_energy;
    int initialised=0;
    int readEnergy(unsigned long long *);
    int init();
public:
    double monitorEnergy();
};