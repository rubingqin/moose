/****************************************************************/
/* MOOSE - Multiphysics Object Oriented Simulation Environment  */
/*                                                              */
/*          All contents are licensed under LGPL V2.1           */
/*             See LICENSE for full restrictions                */
/****************************************************************/

#include "ComputeReducedOrderEigenstrain.h"

#include "MooseMesh.h"
#include "libmesh/quadrature.h"

template <>
InputParameters
validParams<ComputeReducedOrderEigenstrain>()
{
  InputParameters params = validParams<ComputeEigenstrainBase>();
  params.addRequiredParam<std::vector<MaterialPropertyName>>(
      "input_eigenstrain_names", "List of eigenstrains to be applied in this strain calculation");
  return params;
}

ComputeReducedOrderEigenstrain::ComputeReducedOrderEigenstrain(const InputParameters & parameters)
  : ComputeEigenstrainBase(parameters),
    _input_eigenstrain_names(
        getParam<std::vector<MaterialPropertyName>>("input_eigenstrain_names")),
    _eigenstrains(_input_eigenstrain_names.size()),
    _eigenstrains_old(_input_eigenstrain_names.size()),
    _eigenstrain(declareProperty<RankTwoTensor>(_base_name + "reduced_order_eigenstrain")),
    _subproblem(*parameters.get<SubProblem *>("_subproblem")),
    _ncols(1 + _subproblem.mesh().dimension()),
    _second_order(_subproblem.mesh().hasSecondOrderElements()),
    _eigsum(),
    _A(),
    _b(6),
    _AT(),
    _ATb(_ncols),
    _x(6, DenseVector<Real>(_ncols)),
    _vals(6),
    _adjusted_eigenstrain()
{
  for (unsigned int i = 0; i < _eigenstrains.size(); ++i)
  {
    _input_eigenstrain_names[i] = _base_name + _input_eigenstrain_names[i];
    _eigenstrains[i] = &getMaterialProperty<RankTwoTensor>(_input_eigenstrain_names[i]);
    if (_incremental_form)
      _eigenstrains_old[i] = &getMaterialPropertyOld<RankTwoTensor>(_input_eigenstrain_names[i]);
  }
}

void
ComputeReducedOrderEigenstrain::initQpStatefulProperties()
{
  _eigenstrain[_qp].zero();
}

void
ComputeReducedOrderEigenstrain::computeProperties()
{
  if (_incremental_form)
    sumEigenstrainIncremental();
  else
    sumEigenstrainTotal();

  prepareEigenstrain();

  Material::computeProperties();
}

void
ComputeReducedOrderEigenstrain::computeQpEigenstrain()
{
  if (_second_order)
  {
    for (unsigned i = 0; i < 6; ++i)
    {
      _vals[i] = _x[i](0);
      for (unsigned j = 1; j < _ncols; ++j)
        _vals[i] += _x[i](j) * _q_point[_qp](j - 1);
    }
    _adjusted_eigenstrain.fillFromInputVector(_vals);
  }
  _eigenstrain[_qp] = _adjusted_eigenstrain;
}

void
ComputeReducedOrderEigenstrain::sumEigenstrainTotal()
{
  _eigsum.resize(_qrule->n_points());
  for (_qp = 0; _qp < _qrule->n_points(); ++_qp)
  {
    _eigsum[_qp].zero();
    for (auto es : _eigenstrains)
      _eigsum[_qp] += (*es)[_qp];
  }
}

void
ComputeReducedOrderEigenstrain::sumEigenstrainIncremental()
{
  _eigsum.resize(_qrule->n_points());
  for (_qp = 0; _qp < _qrule->n_points(); ++_qp)
  {
    _eigsum[_qp].zero();
    for (unsigned i = 0; i < _eigenstrains.size(); ++i)
    {
      _eigsum[_qp] += (*_eigenstrains[i])[_qp];
      _eigsum[_qp] -= (*_eigenstrains_old[i])[_qp];
    }
  }
}

void
ComputeReducedOrderEigenstrain::prepareEigenstrain()
{
  // The eigenstrains can either be constant in an element or linear in x, y, z
  // If constant, do volume averaging.
  if (!_second_order)
  {
    // Volume average
    _adjusted_eigenstrain.zero();
    Real vol = 0.0;
    for (unsigned qp = 0; qp < _qrule->n_points(); ++qp)
    {
      _adjusted_eigenstrain += _eigsum[qp] * _JxW[qp] * _coord[qp];
      vol += _JxW[qp] * _coord[qp];
    }
    _adjusted_eigenstrain /= vol;
  }
  else
  {
    // 1 x y z

    // Six sets (one for each unique component of eigen) of qp data
    _A.resize(_qrule->n_points(), _ncols);
    for (auto && b : _b)
      b.resize(_qrule->n_points());

    for (unsigned qp = 0; qp < _qrule->n_points(); ++qp)
    {
      _A(qp, 0) = 1.0;
      for (unsigned j = 1; j < _ncols; ++j)
        _A(qp, j) = _q_point[qp](j - 1);

      _b[0](qp) = _eigsum[qp](0, 0);
      _b[1](qp) = _eigsum[qp](1, 1);
      _b[2](qp) = _eigsum[qp](2, 2);
      _b[3](qp) = _eigsum[qp](1, 2);
      _b[4](qp) = _eigsum[qp](0, 2);
      _b[5](qp) = _eigsum[qp](0, 1);
    }

    _A.get_transpose(_AT);
    _A.left_multiply(_AT);
    for (unsigned i = 0; i < 6; ++i)
    {
      _AT.vector_mult(_ATb, _b[i]);

      _A.cholesky_solve(_ATb, _x[i]);
    }
  }
}
