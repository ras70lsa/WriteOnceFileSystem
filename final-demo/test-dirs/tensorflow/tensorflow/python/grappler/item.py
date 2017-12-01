# Copyright 2017 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
"""A python interface for Grappler items."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.core.grappler.costs import op_performance_data_pb2
from tensorflow.core.protobuf import meta_graph_pb2
from tensorflow.python import pywrap_tensorflow as tf_item
from tensorflow.python.framework import errors


class Item(object):
  """GrapplerItem."""

  def __init__(self,
               metagraph,
               ignore_colocation=True,
               ignore_user_placement=False):
    """Creates an Item.

    Args:
      metagraph: a TensorFlow metagraph.
      ignore_colocation: if set, the tool will ignore all the colocation
        constraints generated by TensorFlow.
      ignore_user_placement: if set, all the placement annotations annotated in
        the metagraph will be ignored.
    Raises:
      ValueError: the metagraph is incomplete or invalid.
    """
    self._metagraph = metagraph
    self._item_graph = meta_graph_pb2.MetaGraphDef()
    self._item_graph.CopyFrom(metagraph)
    self._ignore_colocation = ignore_colocation
    self._ignore_user_placement = ignore_user_placement
    self._tf_item = None
    self._BuildTFItem()

  def IdentifyImportantOps(self):
    return tf_item.TF_IdentifyImportantOps(self.tf_item)

  def GetOpProperties(self):
    ret_from_swig = tf_item.TF_GetOpProperties(self.tf_item)
    properties = {}
    for key, values in ret_from_swig.items():
      prop = []
      for value in values:
        prop.append(
            op_performance_data_pb2.OpInfo.TensorProperties.FromString(value))
      properties[key] = prop
    return properties

  @property
  def metagraph(self):
    return self._metagraph

  @property
  def tf_item(self):
    if self._item_graph != self._metagraph:
      self._BuildTFItem()
      self._item_graph.CopyFrom(self._metagraph)
    return self._tf_item

  def _BuildTFItem(self):
    with errors.raise_exception_on_not_ok_status() as status:
      self._tf_item = tf_item.TF_NewItem(self._metagraph.SerializeToString(),
                                         self._ignore_colocation,
                                         self._ignore_user_placement, status)