class EnergyMonitor{
private:
    int readEnergy(unsigned long long *);
public:
    unsigned long long max_energy;
    unsigned long long energy;
    void monitorEnergy();
};