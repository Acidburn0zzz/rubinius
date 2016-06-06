require File.expand_path('../../../spec_helper', __FILE__)
require File.expand_path('../fixtures', __FILE__)

# NOTE: A call to define_finalizer does not guarantee that the
# passed proc or callable will be called at any particular time.
# It is highly questionable whether these aspects of ObjectSpace
# should be spec'd at all.
describe "ObjectSpace.define_finalizer" do
  it "raises an ArgumentError if the object is a Symbol" do
    lambda do
      ObjectSpace.define_finalizer(:symbol, lambda { })
    end.should raise_error(ArgumentError)
  end

  it "raises an ArgumentError if the object is a Fixnum" do
    lambda do
      ObjectSpace.define_finalizer(1, lambda { })
    end.should raise_error(ArgumentError)
  end

  it "raises an RuntimeError if the object is a Bignum" do
    lambda do
      ObjectSpace.define_finalizer(bignum_value(), lambda { })
    end.should raise_error(RuntimeError)
  end

  it "raises an ArgumentError if the object is a Float" do
    lambda do
      ObjectSpace.define_finalizer(3.5, lambda { })
    end.should raise_error(ArgumentError)
  end

  it "raises an ArgumentError if the object is a nil" do
    lambda do
      ObjectSpace.define_finalizer(nil, lambda { })
    end.should raise_error(ArgumentError)
  end

  it "raises a RuntimeError if the object is frozen" do
    lambda do
      ObjectSpace.define_finalizer("abc".freeze, lambda { })
    end.should raise_error(RuntimeError)
  end

  it "raises an ArgumentError if the action does not respond to call" do
    lambda {
      ObjectSpace.define_finalizer("", mock("ObjectSpace.define_finalizer no #call"))
    }.should raise_error(ArgumentError)
  end

  it "accepts an object and a proc" do
    handler = lambda { |obj| obj }
    ObjectSpace.define_finalizer("garbage", handler).should == [0, handler]
  end

  it "accepts an object and a callable" do
    handler = mock("callable")
    def handler.call(obj) end
    ObjectSpace.define_finalizer("garbage", handler).should == [0, handler]
  end

  # see [ruby-core:24095]
  with_feature :fork do
    before :each do
      @fname = tmp("finalizer_test.txt")
      @contents = "finalized"
    end

    after :each do
      rm_r @fname
    end

    it "calls finalizer on process termination" do
      if Kernel::fork then
        loop { break if File.exists?(@fname) }
        IO.read(@fname).should == @contents
      else
        handler = Proc.new do
          File.open(@fname, "w") { |f| f.write(@contents) }
        end
        obj = "Test"
        ObjectSpace.define_finalizer(obj, handler)
        exit 0
      end
    end

    it "calls finalizer at exit even if it is self-referencing" do
      if Kernel::fork then
        loop { break if File.exists?(@fname) }
        IO.read(@fname).should == @contents
      else
        obj = "Test"
        handler = Proc.new do
          obj
          File.open(@fname, "w") { |f| f.write(@contents) }
        end
        ObjectSpace.define_finalizer(obj, handler)
        exit 0
      end
    end

    # These specs are defined under the fork specs because there is no
    # deterministic way to force finalizers to be run, except process exit, so
    # we rely on that.
    it "allows multiple finalizers with different 'callables' to be defined" do
      rd1, wr1 = IO.pipe
      rd2, wr2 = IO.pipe

      if Kernel::fork then
        wr1.close
        wr2.close

        rd1.read.should == "finalized1"
        rd2.read.should == "finalized2"

        rd1.close
        rd2.close
      else
        rd1.close
        rd2.close
        obj = mock("ObjectSpace.define_finalizer multiple")

        ObjectSpace.define_finalizer(obj, Proc.new { wr1.write "finalized1"; wr1.close })
        ObjectSpace.define_finalizer(obj, Proc.new { wr2.write "finalized2"; wr2.close })

        exit 0
      end
    end
  end
end
