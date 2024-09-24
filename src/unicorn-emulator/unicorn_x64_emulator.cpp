#define UNICORN_EMULATOR_IMPL
#include "unicorn_x64_emulator.hpp"

#include "unicorn_memory_regions.hpp"
#include "unicorn_hook.hpp"

#include "function_wrapper.hpp"

namespace unicorn
{
	namespace
	{
		static_assert(static_cast<uint32_t>(memory_permission::none) == UC_PROT_NONE);
		static_assert(static_cast<uint32_t>(memory_permission::read) == UC_PROT_READ);
		static_assert(static_cast<uint32_t>(memory_permission::exec) == UC_PROT_EXEC);
		static_assert(static_cast<uint32_t>(memory_permission::all) == UC_PROT_ALL);

		static_assert(static_cast<uint32_t>(x64_register::end) == UC_X86_REG_ENDING);

		uc_x86_insn map_hookable_instruction(const x64_hookable_instructions instruction)
		{
			switch (instruction)
			{
			case x64_hookable_instructions::syscall:
				return UC_X86_INS_SYSCALL;
			case x64_hookable_instructions::cpuid:
				return UC_X86_INS_CPUID;
			case x64_hookable_instructions::rdtsc:
				return UC_X86_INS_RDTSC;
			case x64_hookable_instructions::rdtscp:
				return UC_X86_INS_RDTSCP;
			default:
				throw std::runtime_error("Bad instruction for mapping");
			}
		}

		memory_violation_type map_memory_violation_type(const uc_mem_type mem_type)
		{
			switch (mem_type)
			{
			case UC_MEM_READ_PROT:
			case UC_MEM_WRITE_PROT:
			case UC_MEM_FETCH_PROT:
				return memory_violation_type::protection;
			case UC_MEM_READ_UNMAPPED:
			case UC_MEM_WRITE_UNMAPPED:
			case UC_MEM_FETCH_UNMAPPED:
				return memory_violation_type::unmapped;
			default:
				throw std::runtime_error("Memory type does not constitute a violation");
			}
		}

		memory_operation map_memory_operation(const uc_mem_type mem_type)
		{
			switch (mem_type)
			{
			case UC_MEM_READ:
			case UC_MEM_READ_PROT:
			case UC_MEM_READ_UNMAPPED:
				return memory_operation::read;
			case UC_MEM_WRITE:
			case UC_MEM_WRITE_PROT:
			case UC_MEM_WRITE_UNMAPPED:
				return memory_operation::write;
			case UC_MEM_FETCH:
			case UC_MEM_FETCH_PROT:
			case UC_MEM_FETCH_UNMAPPED:
				return memory_operation::exec;
			default:
				return memory_operation::none;
			}
		}

		struct hook_object : object
		{
			emulator_hook* as_opaque_hook()
			{
				return reinterpret_cast<emulator_hook*>(this);
			}
		};

		class hook_container : public hook_object
		{
		public:
			template <typename T>
				requires(std::is_base_of_v<object, T>
					&& std::is_move_constructible_v<T>)
			void add(T data, unicorn_hook hook)
			{
				hook_entry entry{};

				entry.data = std::make_unique<T>(std::move(data));
				entry.hook = std::move(hook);

				this->hooks_.emplace_back(std::move(entry));
			}

		private:
			struct hook_entry
			{
				std::unique_ptr<object> data{};
				unicorn_hook hook{};
			};

			std::vector<hook_entry> hooks_;
		};

		class uc_context_serializer
		{
		public:
			uc_context_serializer(uc_engine* uc, const bool in_place)
				: uc_(uc)
			{
				uc_ctl_context_mode(uc, UC_CTL_CONTEXT_CPU | (in_place ? UC_CTL_CONTEXT_MEMORY : 0));

				this->size_ = uc_context_size(uc);
				uce(uc_context_alloc(uc, &this->context_));
			}

			~uc_context_serializer()
			{
				if (this->context_)
				{
					(void)uc_context_free(this->context_);
				}
			}

			void serialize(utils::buffer_serializer& buffer) const
			{
				uce(uc_context_save(this->uc_, this->context_));
				buffer.write(this->context_, this->size_);
			}

			void deserialize(utils::buffer_deserializer& buffer) const
			{
				buffer.read(this->context_, this->size_);
				uce(uc_context_restore(this->uc_, this->context_));
			}

			uc_context_serializer(uc_context_serializer&&) = delete;
			uc_context_serializer(const uc_context_serializer&) = delete;
			uc_context_serializer& operator=(uc_context_serializer&&) = delete;
			uc_context_serializer& operator=(const uc_context_serializer&) = delete;

		private:
			uc_engine* uc_{};
			uc_context* context_{};
			size_t size_{};
		};

		void add_read_hook(uc_engine* uc, const uint64_t address, const size_t size, hook_container& container,
		                   const std::shared_ptr<complex_memory_hook_callback>& callback)
		{
			function_wrapper<void, uc_engine*, uc_mem_type, uint64_t, int, int64_t> wrapper(
				[callback](uc_engine*, const uc_mem_type type, const uint64_t address, const int size,
				           const int64_t)
				{
					const auto operation = map_memory_operation(type);
					if (operation != memory_permission::none)
					{
						(*callback)(address, static_cast<uint64_t>(size), 0, operation);
					}
				});

			unicorn_hook hook{uc};

			uce(uc_hook_add(uc, hook.make_reference(), UC_HOOK_MEM_READ, wrapper.get_function(),
			                wrapper.get_user_data(), address, address + size));

			container.add(std::move(wrapper), std::move(hook));
		}

		void add_write_hook(uc_engine* uc, const uint64_t address, const size_t size, hook_container& container,
		                    const std::shared_ptr<complex_memory_hook_callback>& callback)
		{
			function_wrapper<void, uc_engine*, uc_mem_type, uint64_t, int, int64_t> wrapper(
				[callback](uc_engine*, const uc_mem_type type, const uint64_t address, const int size,
				           const uint64_t value)
				{
					const auto operation = map_memory_operation(type);
					if (operation != memory_permission::none)
					{
						(*callback)(address, static_cast<uint64_t>(size), value, operation);
					}
				});

			unicorn_hook hook{uc};

			uce(uc_hook_add(uc, hook.make_reference(), UC_HOOK_MEM_WRITE, wrapper.get_function(),
			                wrapper.get_user_data(), address, address + size));

			container.add(std::move(wrapper), std::move(hook));
		}

		void add_exec_hook(uc_engine* uc, const uint64_t address, const size_t size, hook_container& container,
		                   const std::shared_ptr<complex_memory_hook_callback>& callback)
		{
			function_wrapper<void, uc_engine*, uint64_t, uint32_t> wrapper(
				[callback](uc_engine*, const uint64_t address, const uint32_t size)
				{
					(*callback)(address, size, 0, memory_permission::exec);
				});

			unicorn_hook hook{uc};

			uce(uc_hook_add(uc, hook.make_reference(), UC_HOOK_CODE, wrapper.get_function(),
			                wrapper.get_user_data(), address, address + size));

			container.add(std::move(wrapper), std::move(hook));
		}

		basic_block map_block(const uc_tb& translation_block)
		{
			basic_block block{};

			block.address = translation_block.pc;
			block.instruction_count = translation_block.icount;
			block.size = translation_block.size;

			return block;
		}

		class unicorn_x64_emulator : public x64_emulator
		{
		public:
			unicorn_x64_emulator()
			{
				uce(uc_open(UC_ARCH_X86, UC_MODE_64, &this->uc_));
			}

			~unicorn_x64_emulator() override
			{
				this->hooks_.clear();
				uc_close(this->uc_);
			}

			void start(uint64_t start, const uint64_t end, std::chrono::microseconds timeout,
			           const size_t count) override
			{
				while (true)
				{
					if (timeout.count() < 0)
					{
						timeout = {};
					}

					this->retry_after_violation_ = false;
					const auto res = uc_emu_start(*this, start, end, static_cast<uint64_t>(timeout.count()), count);
					if (res == UC_ERR_OK)
					{
						return;
					}

					const auto is_violation = res == UC_ERR_READ_UNMAPPED || //
						res == UC_ERR_WRITE_UNMAPPED || //
						res == UC_ERR_FETCH_UNMAPPED || //
						res == UC_ERR_READ_PROT || //
						res == UC_ERR_WRITE_PROT || //
						res == UC_ERR_FETCH_PROT;

					if (is_violation && this->retry_after_violation_)
					{
						start = this->read_instruction_pointer();
						continue;
					}

					uce(res);
				}
			}

			void stop() override
			{
				uce(uc_emu_stop(*this));
			}

			void write_raw_register(const int reg, const void* value, const size_t size) override
			{
				size_t result_size = size;
				uce(uc_reg_write2(*this, reg, value, &result_size));

				if (size < result_size)
				{
					throw std::runtime_error(
						"Register size mismatch: " + std::to_string(size) + " != " + std::to_string(result_size));
				}
			}

			void read_raw_register(const int reg, void* value, const size_t size) override
			{
				size_t result_size = size;
				memset(value, 0, size);
				uce(uc_reg_read2(*this, reg, value, &result_size));

				if (size < result_size)
				{
					throw std::runtime_error(
						"Register size mismatch: " + std::to_string(size) + " != " + std::to_string(result_size));
				}
			}

			void map_memory(const uint64_t address, const size_t size, memory_permission permissions) override
			{
				uce(uc_mem_map(*this, address, size, static_cast<uint32_t>(permissions)));
			}

			void unmap_memory(const uint64_t address, const size_t size) override
			{
				uce(uc_mem_unmap(*this, address, size));
			}

			bool try_read_memory(const uint64_t address, void* data, const size_t size) const override
			{
				return uc_mem_read(*this, address, data, size) == UC_ERR_OK;
			}

			void read_memory(const uint64_t address, void* data, const size_t size) const override
			{
				uce(uc_mem_read(*this, address, data, size));
			}

			void write_memory(const uint64_t address, const void* data, const size_t size) override
			{
				uce(uc_mem_write(*this, address, data, size));
			}

			void apply_memory_protection(const uint64_t address, const size_t size,
			                             memory_permission permissions) override
			{
				uce(uc_mem_protect(*this, address, size, static_cast<uint32_t>(permissions)));
			}

			/*std::vector<memory_region> get_memory_regions() override
			{
				const unicorn_memory_regions regions{*this};
				const auto region_span = regions.get_span();

				std::vector<memory_region> result{};
				result.reserve(region_span.size());

				for (const auto region : region_span)
				{
					memory_region reg{};
					reg.start = region.begin;
					reg.length = region.end - region.begin;
					reg.committed = true;
					reg.pemissions = static_cast<memory_permission>(region.perms) & memory_permission::all;

					result.push_back(reg);
				}

				return result;
			}*/

			emulator_hook* hook_instruction(int instruction_type,
			                                instruction_hook_callback callback)
			{
				function_wrapper<int, uc_engine*> wrapper([c = std::move(callback)](uc_engine*)
				{
					return (c() == instruction_hook_continuation::skip_instruction)
						       ? 1
						       : 0;
				});

				unicorn_hook hook{*this};
				auto container = std::make_unique<hook_container>();

				const auto inst_type = static_cast<x64_hookable_instructions>(instruction_type);

				if (inst_type == x64_hookable_instructions::invalid)
				{
					uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_INSN_INVALID, wrapper.get_function(),
					                wrapper.get_user_data(), 0, std::numeric_limits<pointer_type>::max()));
				}
				else
				{
					const auto uc_instruction = map_hookable_instruction(inst_type);
					uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_INSN, wrapper.get_function(),
					                wrapper.get_user_data(), 0, std::numeric_limits<pointer_type>::max(),
					                uc_instruction));
				}

				container->add(std::move(wrapper), std::move(hook));

				auto* result = container->as_opaque_hook();

				this->hooks_.push_back(std::move(container));

				return result;
			}

			emulator_hook* hook_basic_block(basic_block_hook_callback callback) override
			{
				function_wrapper<void, uc_engine*, uint64_t, size_t> wrapper(
					[c = std::move(callback)](uc_engine*, const uint64_t address, const size_t size)
					{
						basic_block block{};
						block.address = address;
						block.size = size;

						c(block);
					});

				unicorn_hook hook{*this};
				auto container = std::make_unique<hook_container>();

				uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_BLOCK, wrapper.get_function(),
				                wrapper.get_user_data(), 0, std::numeric_limits<pointer_type>::max())
				);

				container->add(std::move(wrapper), std::move(hook));

				auto* result = container->as_opaque_hook();
				this->hooks_.push_back(std::move(container));
				return result;
			}

			emulator_hook* hook_edge_generation(edge_generation_hook_callback callback) override
			{
				function_wrapper<void, uc_engine*, uc_tb*, uc_tb*> wrapper(
					[c = std::move(callback)](uc_engine*, const uc_tb* cur_tb, const uc_tb* prev_tb)
					{
						const auto current_block = map_block(*cur_tb);
						const auto previous_block = map_block(*prev_tb);

						c(current_block, previous_block);
					});

				unicorn_hook hook{*this};
				auto container = std::make_unique<hook_container>();

				uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_EDGE_GENERATED, wrapper.get_function(),
				                wrapper.get_user_data(), 0, std::numeric_limits<pointer_type>::max())
				);

				container->add(std::move(wrapper), std::move(hook));

				auto* result = container->as_opaque_hook();
				this->hooks_.push_back(std::move(container));
				return result;
			}

			emulator_hook* hook_interrupt(interrupt_hook_callback callback) override
			{
				function_wrapper<void, uc_engine*, int> wrapper(
					[c = std::move(callback)](uc_engine*, const int interrupt_type)
					{
						c(interrupt_type);
					});

				unicorn_hook hook{*this};
				auto container = std::make_unique<hook_container>();

				uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_INTR, wrapper.get_function(),
				                wrapper.get_user_data(), 0, std::numeric_limits<pointer_type>::max())
				);

				container->add(std::move(wrapper), std::move(hook));

				auto* result = container->as_opaque_hook();
				this->hooks_.push_back(std::move(container));
				return result;
			}

			emulator_hook* hook_memory_violation(uint64_t address, size_t size,
			                                     memory_violation_hook_callback callback) override
			{
				function_wrapper<bool, uc_engine*, uc_mem_type, uint64_t, int, int64_t> wrapper(
					[c = std::move(callback), this](uc_engine*, const uc_mem_type type,
					                                const uint64_t address, const int size, const int64_t)
					{
						const auto ip = this->read_instruction_pointer();

						assert(size >= 0);
						const auto operation = map_memory_operation(type);
						const auto violation = map_memory_violation_type(type);

						const auto resume = c(address, static_cast<uint64_t>(size), operation, violation) ==
							memory_violation_continuation::resume;

						const auto has_ip_changed = ip != this->read_instruction_pointer();

						if (!resume)
						{
							return false;
						}

						this->retry_after_violation_ = resume && has_ip_changed;

						if (has_ip_changed)
						{
							return false;
						}

						return true;
					});

				unicorn_hook hook{*this};
				auto container = std::make_unique<hook_container>();

				uce(uc_hook_add(*this, hook.make_reference(), UC_HOOK_MEM_INVALID, wrapper.get_function(),
				                wrapper.get_user_data(), address, size));

				container->add(std::move(wrapper), std::move(hook));

				auto* result = container->as_opaque_hook();
				this->hooks_.push_back(std::move(container));
				return result;
			}

			emulator_hook* hook_memory_access(const uint64_t address, const size_t size, const memory_operation filter,
			                                  complex_memory_hook_callback callback) override
			{
				if (filter == memory_permission::none)
				{
					return nullptr;
				}

				const auto shared_callback = std::make_shared<complex_memory_hook_callback>(std::move(callback));

				auto container = std::make_unique<hook_container>();

				if ((filter & memory_operation::read) != memory_operation::none)
				{
					add_read_hook(*this, address, size, *container, shared_callback);
				}

				if ((filter & memory_operation::write) != memory_operation::none)
				{
					add_write_hook(*this, address, size, *container, shared_callback);
				}

				if ((filter & memory_operation::exec) != memory_operation::none)
				{
					add_exec_hook(*this, address, size, *container, shared_callback);
				}

				auto* result = container->as_opaque_hook();

				this->hooks_.push_back(std::move(container));

				return result;
			}

			void delete_hook(emulator_hook* hook) override
			{
				const auto entry = std::ranges::find_if(this->hooks_, [&](const std::unique_ptr<hook_object>& hook_ptr)
				{
					return hook_ptr->as_opaque_hook() == hook;
				});

				if (entry != this->hooks_.end())
				{
					this->hooks_.erase(entry);
				}
			}

			operator uc_engine*() const
			{
				return this->uc_;
			}

			void serialize_state(utils::buffer_serializer& buffer, const bool is_snapshot) const override
			{
				if (this->has_snapshots_ && !is_snapshot)
				{
					// TODO: Investigate if this is really necessary
					throw std::runtime_error("Unable to serialize after snapshot was taken!");
				}

				this->has_snapshots_ |= is_snapshot;

				const uc_context_serializer serializer(this->uc_, is_snapshot);
				serializer.serialize(buffer);
			}

			void deserialize_state(utils::buffer_deserializer& buffer, const bool is_snapshot) override
			{
				if (this->has_snapshots_ && !is_snapshot)
				{
					// TODO: Investigate if this is really necessary
					throw std::runtime_error("Unable to deserialize after snapshot was taken!");
				}

				const uc_context_serializer serializer(this->uc_, is_snapshot);
				serializer.deserialize(buffer);
			}

		private:
			mutable bool has_snapshots_{false};
			uc_engine* uc_{};
			bool retry_after_violation_{false};
			std::vector<std::unique_ptr<hook_object>> hooks_{};
		};
	}

	std::unique_ptr<x64_emulator> create_x64_emulator()
	{
		return std::make_unique<unicorn_x64_emulator>();
	}
}