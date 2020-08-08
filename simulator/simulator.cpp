#include "gui.h"
#include "pi.h"
#include "scalar.h"
#include "sim_params.h"
#include "sine_series.h"
#include "wrappers/sdl_context.h"
#include "wrappers/sdl_imgui.h"
#include "wrappers/sdl_imgui_context.h"
#include <Eigen/Dense>
#include <absl/strings/str_format.h>
#include <array>
#include <glad/glad.h>
#include <implot.h>
#include <iostream>

int to_gate_enum(bool command) { return command ? HIGH : LOW; }

void update_gate_state(const Scalar dead_time, const Scalar current_time,
                       GateState* gate_state) {
    for (int i = 0; i < 3; ++i) {
        const int command = gate_state->commanded[i] ? HIGH : LOW;

        if (gate_state->actual[i] == command) {
            // nothing to do here
            continue;
        }

        if (gate_state->actual[i] == OFF) {
            // see if sufficient dead time has been acheived
            if (current_time - gate_state->last_commutation_times[i] >
                dead_time) {
                gate_state->actual[i] = command;
            }
        } else {
            // gate is actually on - need to enter dead time
            gate_state->actual[i] = OFF;
            gate_state->last_commutation_times[i] = current_time;
        }
    }
}

Scalar get_back_emf(const Eigen::Matrix<Scalar, 5, 1>& normalized_bEmf_coeffs,
                    const Scalar electrical_angle) {
    Eigen::Matrix<Scalar, 5, 1> sines;
    generate_odd_sine_series(/*num_terms=*/sines.rows(), electrical_angle,
                             sines.data());
    return sines.dot(normalized_bEmf_coeffs);
}

bool get_commutation_state(Scalar progress) {
    while (progress <= 0) {
        progress += 1;
    }
    while (progress > 1) {
        progress -= 1;
    }
    return (progress >= 0.5);
}

std::array<bool, 3> six_step_commutate(const Scalar electrical_angle,
                                       const Scalar phase_advance) {
    const Scalar progress = std::fmod(electrical_angle, 2 * kPI) / (2 * kPI);

    return {get_commutation_state(progress + phase_advance),
            get_commutation_state(progress + phase_advance - 1.0 / 3),
            get_commutation_state(progress + phase_advance - 2.0 / 3)};
}

void step(const SimParams& params, SimState* state) {
    update_gate_state(params.gate_dead_time, state->time, &state->gate_state);

    // apply pole voltages
    for (int i = 0; i < 3; ++i) {
        Scalar v_pole = 0;
        switch (state->gate_state.actual[i]) {
        case OFF:
            // todo: derivation
            if (state->coil_currents(i) > 0) {
                state->pole_voltages(i) = 0;
            } else {
                state->pole_voltages(i) = params.bus_voltage;
            }
            if (std::abs(state->coil_currents(i)) >
                params.diode_active_current) {
                state->pole_voltages(i) -= params.diode_active_voltage;
            }
            break;
        case HIGH:
            state->pole_voltages(i) = params.bus_voltage;
            break;
        case LOW:
            state->pole_voltages(i) = 0;
            break;
        default:
            printf("Unhandled switch case!");
            exit(-1);
        }
    }

    // compute normalized back emfs
    Eigen::Matrix<Scalar, 3, 1> normalized_bEmfs;
    normalized_bEmfs << // clang-format off
        get_back_emf(params.normalized_bEmf_coeffs, state->electrical_angle),
        get_back_emf(params.normalized_bEmf_coeffs,
                     state->electrical_angle - 2 * kPI / 3),
        get_back_emf(params.normalized_bEmf_coeffs,
                     state->electrical_angle - 4 * kPI / 3); // clang-format on

    state->bEmfs = normalized_bEmfs * state->rotor_angular_vel;

    // compute neutral point voltage
    // todo: derivation
    state->neutral_voltage =
        (state->pole_voltages.sum() - state->bEmfs.sum()) / 3;

    for (int i = 0; i < 3; ++i) {
        state->phase_voltages(i) =
            state->pole_voltages(i) - state->neutral_voltage;
    }

    Eigen::Matrix<Scalar, 3, 1> di_dt;
    for (int i = 0; i < 3; ++i) {
        di_dt(i) = (state->phase_voltages(i) - state->bEmfs(i) -
                    state->coil_currents(i) * params.phase_resistance) /
                   params.phase_inductance;
    }

    state->coil_currents += di_dt * params.dt;

    state->torque = state->coil_currents.dot(normalized_bEmfs);

    state->rotor_angular_accel = state->torque / params.rotor_inertia;
    state->rotor_angular_vel += state->rotor_angular_accel * params.dt;
    state->rotor_angle += state->rotor_angular_vel * params.dt;
    state->rotor_angle = std::fmod(state->rotor_angle, 2 * kPI);
    if (state->rotor_angle < 0) {
        state->rotor_angle += 2 * kPI;
    }

    state->electrical_angle = state->rotor_angle * params.num_pole_pairs;
    state->electrical_angle = std::fmod(state->electrical_angle, 2 * kPI);

    state->time += params.dt;
}

using namespace biro;

int main(int argc, char* argv[]) {
    SimParams params;
    init_sim_params(&params);

    SimState state;
    init_sim_state(&state);

    VizData viz_data;
    init_viz_data(&viz_data);

    wrappers::SdlContext sdl_context("Motor Simulator",
                                     /*width=*/1920 / 2,
                                     /*height=*/1080 / 2);

    if (sdl_context.status_ != 0) {
        return -1;
    }

    if (gladLoadGL() == 0) {
        printf("Failed to initialize OpenGL loader!\n");
        exit(-1);
    }

    wrappers::SdlImguiContext imgui_context(sdl_context.window_,
                                            sdl_context.gl_context_);

    bool quit_flag = false;
    while (!quit_flag) {
        quit_flag = wrappers::process_sdl_imgui_events(sdl_context.window_);
        if (quit_flag) {
            break;
        }

        wrappers::sdl_imgui_newframe(sdl_context.window_);

        run_gui(&params, &state, &viz_data);
        if (!params.paused) {
            for (int i = 0; i < params.step_multiplier; ++i) {
                state.gate_state.commanded = six_step_commutate(
                    state.electrical_angle, state.six_step_phase_advance);
                step(params, &state);
            }
        }

        ImGui::ShowDemoWindow();
        ImPlot::ShowDemoWindow();

        ImGui::Render();
        int display_w, display_h;
        SDL_GetWindowSize(sdl_context.window_, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.0, 0.0, 0.0, 0.0);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(sdl_context.window_);
    }

    return 0;
}
