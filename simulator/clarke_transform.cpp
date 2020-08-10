#include "clarke_transform.h"
#include <cmath>

const Scalar kClarkeScale = std::sqrt(2.0 / 3);

Eigen::Matrix<Scalar, 3, 3> make_clarke_transform() {
    const Scalar sqrt2 = std::sqrt(2.0);
    const Scalar sqrt3 = std::sqrt(3.0);
    Eigen::Matrix<Scalar, 3, 3> clarke_transform;
    clarke_transform <<
        // clang-format off
	1,        -1.0/2,   -1.0/2,
	0,       sqrt3/2, -sqrt3/2,
	1/sqrt2, 1/sqrt2,  1/sqrt2
        // clang-format on
        ;
    clarke_transform *= sqrt2 / sqrt3;
    return clarke_transform;
}

const Eigen::Matrix<Scalar, 3, 3> kClarkeTransform3x3 = make_clarke_transform();
const Eigen::Matrix<Scalar, 2, 3> kClarkeTransform2x3 =
    make_clarke_transform().block<2, 3>(0, 0);
