#ifndef CIRCULAR_BUFFER_VECTOR_H
#define CIRCULAR_BUFFER_VECTOR_H
//
#include <vector>

using std::vector;


class CIRCULAR_BUFFER_VECTOR{
public:
    // Dimensions
    size_t buffer_size;
    size_t vector_size;

    //
    CIRCULAR_BUFFER_VECTOR(void);
    CIRCULAR_BUFFER_VECTOR(size_t buffer_size_in, size_t vector_size_in);
    // Initiate and reset the data in the buffer
    void Init(size_t buffer_size_in, const vector<float> &initial_value); // If using the first type of constructer, this function helps
    void Reset(const vector<float> &value); // Reset all the elements without changeing the buffer size
    // Element-wise operation
    vector<float> Get(int i); // Get the element that is i samples ago
    void Set(int i, const vector<float> &value); // Set the element that is i samples ago to the "value"
    void Increase(int i, const vector<float> &increase_value, bool is_minus); // vi += (or -=) increase_value
    // Iterate the buffer
    void Insert(const vector<float> &x_new); // Pop the oldest element and push a new element

private:
    int idx; // The index of the current data in the buffer

    vector<vector<float> > buffer;

    // Utilities
    // Increment
    void Get_VectorIncrement(vector<float> &v_a, const vector<float> &v_b, bool is_minus); // v_a += (or -=) v_b

};

#endif
