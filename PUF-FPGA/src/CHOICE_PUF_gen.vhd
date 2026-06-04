
library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

entity CHOICE_PUF_gen is
  generic(
           PUF_WIDTH : integer := 32
         );
  Port ( 
         clk             : in STD_LOGIC;
         ff_reset        : in STD_LOGIC;
         chip_enable     : in STD_LOGIC;
      ASR_length_conf : in STD_LOGIC_VECTOR(11 downto 0);
         ASR_data_conf   : in STD_LOGIC_VECTOR(3 downto 0);
         puf_response    : out STD_LOGIC_VECTOR((PUF_WIDTH -1) downto 0)
       );
end CHOICE_PUF_gen;

architecture Behavioral of Choice_PUF_gen is
  component Choice_PUF is
    Port ( 
           clk             : in STD_LOGIC;
           chip_enable     : in STD_LOGIC;
           ff_reset        : in STD_LOGIC;
          ASR_length_conf : in STD_LOGIC_VECTOR(11 downto 0);
           ASR_data_conf   : in STD_LOGIC_VECTOR(3 downto 0);
           puf_bit         : out STD_LOGIC
         );
  end component;

  -- Internal signals for each PUF bit with S constraint to prevent trimming
  signal puf_bits : STD_LOGIC_VECTOR((PUF_WIDTH - 1) downto 0);
  
  attribute syn_dont_touch : integer;
  attribute syn_dont_touch of puf_bits : signal is 1;
  
  -- Prevent cross-instance optimization/merging
  attribute KEEP_HIERARCHY : string;

begin
  GEN_PUF:
  for i in 0 to PUF_WIDTH-1 generate
    -- Apply KEEP_HIERARCHY to each PUF instance to prevent merging
    attribute KEEP_HIERARCHY of PUF : label is "TRUE";
    attribute syn_dont_touch of PUF : label is 1;
  begin
    -- Instantiate PUF - outputs are connected to puf_bits which has DONT_TOUCH
    PUF : CHOICE_PUF port map
    (
      clk              => clk,
      chip_enable      => chip_enable,
      ff_reset         => ff_reset,
      ASR_length_conf  => ASR_length_conf,
      ASR_data_conf    => ASR_data_conf,
      puf_bit          => puf_bits(i)
    );
  end generate GEN_PUF;
  
  -- Connect internal signals to output
  puf_response <= puf_bits;

end Behavioral;