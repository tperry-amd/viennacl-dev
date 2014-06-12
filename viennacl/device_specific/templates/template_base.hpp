#ifndef VIENNACL_DEVICE_SPECIFIC_TEMPLATES_TEMPLATE_BASE_BASE
#define VIENNACL_DEVICE_SPECIFIC_TEMPLATES_TEMPLATE_BASE_BASE

/* =========================================================================
   Copyright (c) 2010-2013, Institute for Microelectronics,
                            Institute for Analysis and Scientific Computing,
                            TU Wien.
   Portions of this software are copyright by UChicago Argonne, LLC.

                            -----------------
                  ViennaCL - The Vienna Computing Library
                            -----------------

   Project Head:    Karl Rupp                   rupp@iue.tuwien.ac.at

   (A list of authors and contributors can be found in the PDF manual)

   License:         MIT (X11), see file LICENSE in the base directory
============================================================================= */


/** @file viennacl/generator/profile_base.hpp
 *
 * Base classes for the profiles
*/

#include <list>
#include <set>

#include "viennacl/ocl/kernel.hpp"
#include "viennacl/ocl/device.hpp"
#include "viennacl/ocl/device_utils.hpp"
#include "viennacl/ocl/infos.hpp"

#include "viennacl/scheduler/forwards.h"

#include "viennacl/device_specific/tree_parsing/traverse.hpp"
#include "viennacl/device_specific/tree_parsing/map.hpp"
#include "viennacl/device_specific/tree_parsing/prototype_generation.hpp"
#include "viennacl/device_specific/tree_parsing/set_arguments.hpp"
#include "viennacl/device_specific/tree_parsing/statement_representation.hpp"

namespace viennacl
{

  namespace device_specific
  {

    class template_base
    {
    public:
      class parameters{
      private:
        virtual bool invalid_impl(viennacl::ocl::device const & /*dev*/, size_t /*scalartype_size*/) const { return false; }
        virtual unsigned int lmem_used(unsigned int /*scalartype_size*/) const { return 0; }
      public:
        parameters(const char * scalartype, unsigned int simd_width, unsigned int local_size_1, unsigned int local_size_2, unsigned int num_kernels) :
          scalartype_(scalartype), simd_width_(simd_width), local_size_0_(local_size_1), local_size_1_(local_size_2), num_kernels_(num_kernels){ }

        unsigned int num_kernels() const  { return num_kernels_; }
        std::string const & scalartype() const { return scalartype_; }
        unsigned int local_size_0() const { return local_size_0_; }
        unsigned int local_size_1() const { return local_size_1_; }
        unsigned int simd_width() const { return simd_width_; }

        /** @brief returns whether or not the profile has undefined behavior on particular device */
        bool is_invalid() const
        {
          bool invalid = false;
          viennacl::ocl::device const & dev = viennacl::ocl::current_device();

          //Query device informations
          size_t lmem_available = static_cast<size_t>(dev.local_mem_size());
          unsigned int scalartype_size = utils::scalartype_size(scalartype_);
          invalid |= (lmem_used(scalartype_size)>lmem_available);

          //Invalid work group size
          size_t max_workgroup_size = dev.max_work_group_size();
          std::vector<size_t> max_work_item_sizes = dev.max_work_item_sizes();
          invalid |= local_size_0_*local_size_1_ > max_workgroup_size
              || local_size_0_ > max_work_item_sizes[0]
              || local_size_1_ > max_work_item_sizes[1]; // uses too much resources


          //Not warp multiple
          if(dev.type()==CL_DEVICE_TYPE_GPU){
            unsigned int warp_size = 32;
            if(dev.vendor_id()==4098)
              warp_size = 64;
            invalid |= (((local_size_0_*local_size_1_)%warp_size)>0);
          }

          //Invalid SIMD Width
          invalid |= (simd_width_!=1 && simd_width_!=2 &&
                      simd_width_!=4 && simd_width_!=8 &&
                      simd_width_!=16);

          return  invalid || invalid_impl(dev, scalartype_size);
        }
      protected:
        std::string scalartype_;

        unsigned int simd_width_;
        unsigned int local_size_0_;
        unsigned int local_size_1_;
        unsigned int num_kernels_;
      };

    private:

      /** @brief Generates the body of the associated kernel function */
      virtual void core(unsigned int kernel_id, utils::kernel_generation_stream& stream, statements_container const & statements, std::vector<mapping_type> const & mapping) const = 0;

      /** @brief generates the arguments that are global to the kernel (different from the object-specific arguments) */
      virtual void add_kernel_arguments(statements_container const & statements, std::string & arguments_string) const = 0;

      /** @brief Sets the SIMD width of the mapped objects (Necessary for generating the prototype ) */
      virtual void init_simd_width(mapping_type::value_type const & v) const
      {
        if(mapped_handle * p = dynamic_cast<mapped_handle *>(v.second.get()))
          p->set_simd_width(parameters_.simd_width());
      }

      virtual void configure_impl(unsigned int kernel_id, statements_container const & statements, viennacl::ocl::kernel & kernel, unsigned int & n_arg)  const = 0;

    public:
      /** @brief The constructor */
      template_base(template_base::parameters const & parameters, binding_policy_t binding_policy) : parameters_(parameters), binding_policy_(binding_policy){ }

      /** @brief Generates the code associated with this profile onto the provided stream */
      std::string generate(statements_container const & statements)
      {
        utils::kernel_generation_stream stream;

        //Kernel Prefix
        std::string kernel_prefix = tree_parsing::statements_representation(statements, binding_policy_);

        //Create mapping
        std::vector<mapping_type> mapping(statements.data().size());
        tools::shared_ptr<symbolic_binder> binder = make_binder(binding_policy_);
        for(statements_container::data_type::const_iterator it = statements.data().begin() ; it != statements.data().end() ; ++it)
          tree_parsing::traverse(*it, it->root(), tree_parsing::map_functor(*binder,mapping[std::distance(statements.data().begin(), it)]));
        for(unsigned int i = 0 ; i < mapping.size() ; ++i)
          for(mapping_type::const_iterator it = mapping[i].begin() ; it != mapping[i].end() ; ++it)
              init_simd_width(*it);

        //Generate Prototype
        std::string prototype;
        std::set<std::string> already_generated;
        add_kernel_arguments(statements, prototype);
        for(statements_container::data_type::const_iterator it = statements.data().begin() ; it != statements.data().end() ; ++it)
          tree_parsing::traverse(*it, it->root(), tree_parsing::prototype_generation_traversal(already_generated, prototype, mapping[std::distance(statements.data().begin(), it)]));
        prototype.erase(prototype.size()-1); //Last comma pruned

        for(unsigned int i = 0 ; i < parameters_.num_kernels() ; ++i)
        {
          stream << " __attribute__((reqd_work_group_size(" << parameters_.local_size_0() << "," << parameters_.local_size_1() << "," << 1 << ")))" << std::endl;
          stream << "__kernel " << "void " << kernel_prefix << i << "(" << prototype << ")" << std::endl;
          stream << "{" << std::endl;
          stream.inc_tab();
          core(i, stream, statements, mapping);
          stream.dec_tab();
          stream << "}" << std::endl;
        }

        return stream.str();
      }

      void enqueue(std::string const & program_name, statements_container const & statements)
      {
        viennacl::ocl::program & program = viennacl::ocl::current_context().get_program(program_name);
        std::string prefix = tree_parsing::statements_representation(statements, binding_policy_);

        //Get the kernels
        std::vector<viennacl::ocl::kernel*> kernels(parameters_.num_kernels());
        for(std::vector<viennacl::ocl::kernel*> ::iterator it = kernels.begin() ; it != kernels.end() ; ++it)
           *it = &program.get_kernel(prefix+tools::to_string(std::distance(kernels.begin(), it)));

        //Configure
        for(std::vector<viennacl::ocl::kernel*>::iterator it = kernels.begin() ; it != kernels.end() ; ++it)
        {
          unsigned int current_arg = 0;
          tools::shared_ptr<symbolic_binder> binder = make_binder(binding_policy_);
          (*it)->local_work_size(0,parameters_.local_size_0());
          (*it)->local_work_size(1,parameters_.local_size_1());
          configure_impl(std::distance(kernels.begin(), it), statements, **it, current_arg);
          for(typename statements_container::data_type::const_iterator itt = statements.data().begin() ; itt != statements.data().end() ; ++itt)
            tree_parsing::traverse(*itt, itt->root(), tree_parsing::set_arguments_functor(*binder,current_arg,**it));
        }

        //Enqueue
        for(std::vector<viennacl::ocl::kernel*>::iterator it = kernels.begin() ; it != kernels.end() ; ++it)
          viennacl::ocl::enqueue(**it);
      }

    protected:
      template_base::parameters const & parameters_;
      binding_policy_t binding_policy_;
    };

  }

}

#endif
